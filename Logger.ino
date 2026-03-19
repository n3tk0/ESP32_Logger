/**************************************************************************************************
 * PROJECT: ESP32 Low-Power Water Usage Logger v4.2.0
 * TARGET:  XIAO ESP32-C3 (RISC-V)
 * AUTHOR:  Petko Georgiev
 *
 * МОДУЛНА СТРУКТУРА:
 *   src/core/Config.h/.cpp         – структури, enums, константи
 *   src/core/Globals.h/.cpp        – глобални променливи
 *   src/utils/Utils.h/.cpp         – помощни функции (format, sanitize, path)
 *   src/managers/ConfigManager.h/.cpp  – зареждане/запис на конфигурация
 *   src/managers/WiFiManager.h/.cpp    – WiFi, AP, NTP + safeWiFiShutdown()
 *   src/managers/StorageManager.h/.cpp – LittleFS / SD управление
 *   src/managers/RtcManager.h/.cpp     – DS1302, bootcount backup, wake reason
 *   src/managers/HardwareManager.h/.cpp – init pins, ISR, debounce
 *   src/managers/DataLogger.h/.cpp     – log buffer, flush to FS
 *   src/web/WebServer.h/.cpp       – AsyncWebServer handlers
 *   Logger.ino                     – само setup() и loop()
 **************************************************************************************************/

#define CONFIG_FREERTOS_UNICORE 1

#include <Arduino.h>
#include <esp_sleep.h>

#include "src/core/Globals.h"
#include "src/managers/ConfigManager.h"
#include "src/managers/HardwareManager.h"
#include "src/managers/StorageManager.h"
#include "src/managers/RtcManager.h"
#include "src/managers/WiFiManager.h"
#include "src/managers/DataLogger.h"
#include "src/web/WebServer.h"    // setupWebServer()
#include "src/utils/Utils.h"

// ── Platform v5.0 — multi-sensor modules (compiled in only when needed) ──────
#include "src/sensors/SensorManager.h"
// Existing sensors (upgraded)
#include "src/sensors/plugins/BME280Sensor.h"
#include "src/sensors/plugins/SDS011Sensor.h"
#include "src/sensors/plugins/PMS5003Sensor.h"
#include "src/sensors/plugins/WaterFlowSensor.h"   // replaces YFS201Sensor (YF-S201 + YF-S403)
#include "src/sensors/plugins/ENS160Sensor.h"
#include "src/sensors/plugins/SGP30Sensor.h"
#include "src/sensors/plugins/RainSensor.h"
#include "src/sensors/plugins/WindSensor.h"
// New sensors
#include "src/sensors/plugins/VEML6075Sensor.h"
#include "src/sensors/plugins/VEML7700Sensor.h"
#include "src/sensors/plugins/BH1750Sensor.h"
#include "src/sensors/plugins/SoilMoistureSensor.h"
#include "src/sensors/plugins/SCD4xSensor.h"
#include "src/sensors/plugins/BME688Sensor.h"
#include "src/sensors/plugins/HCSR04Sensor.h"
#include "src/sensors/plugins/DS18B20Sensor.h"
#include "src/pipeline/DataPipeline.h"
#include "src/tasks/TaskManager.h"
#include "src/export/ExportManager.h"
#include "src/export/MqttExporter.h"
#include "src/export/HttpExporter.h"
#include "src/export/SensorCommunityExporter.h"
#include "src/export/OpenSenseMapExporter.h"
#include "src/web/ApiHandlers.h"

// ---------------------------------------------------------------------------
// Detect operating mode from /platform_config.json
// Returns: 0 = legacy (default, no change), 1 = continuous, 2 = hybrid
// ---------------------------------------------------------------------------
static uint8_t _detectPlatformMode() {
    if (!fsAvailable || !activeFS) return 0;
    File f = activeFS->open("/platform_config.json", FILE_READ);
    if (!f) return 0;
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return 0; }
    f.close();
    const char* mode = doc["mode"] | "legacy";
    if (strcmp(mode, "continuous") == 0) return 1;
    if (strcmp(mode, "hybrid")     == 0) return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// Register all sensor plugins and init pipeline (called in continuous/hybrid)
// ---------------------------------------------------------------------------
static void _initPlatform() {
    Serial.println("=== Platform v5.0: initialising sensors ===");

    // Register all plugins
    // Environmental
    sensorManager.registerPlugin("bme280",  []()->ISensor*{ return new BME280Sensor(); });
    sensorManager.registerPlugin("bmp280",  []()->ISensor*{ return new BME280Sensor(); }); // BMP280 variant (no humidity)
    sensorManager.registerPlugin("bme688",  []()->ISensor*{ return new BME688Sensor(); });
    // Air quality
    sensorManager.registerPlugin("sds011",  []()->ISensor*{ return new SDS011Sensor(); });
    sensorManager.registerPlugin("pms5003", []()->ISensor*{ return new PMS5003Sensor(); });
    sensorManager.registerPlugin("ens160",  []()->ISensor*{ return new ENS160Sensor(); });
    sensorManager.registerPlugin("sgp30",   []()->ISensor*{ return new SGP30Sensor(); });
    sensorManager.registerPlugin("scd4x",   []()->ISensor*{ return new SCD4xSensor(); });
    // Light
    sensorManager.registerPlugin("veml6075",[]()->ISensor*{ return new VEML6075Sensor(); });
    sensorManager.registerPlugin("veml7700",[]()->ISensor*{ return new VEML7700Sensor(); });
    sensorManager.registerPlugin("bh1750",  []()->ISensor*{ return new BH1750Sensor(); });
    // Water / flow
    sensorManager.registerPlugin("yfs201",  []()->ISensor*{ return new WaterFlowSensor("yfs201", 450.0f); });
    sensorManager.registerPlugin("yfs403",  []()->ISensor*{ return new WaterFlowSensor("yfs403", 600.0f); });
    // Weather
    sensorManager.registerPlugin("rain",    []()->ISensor*{ return new RainSensor(); });
    sensorManager.registerPlugin("wind",    []()->ISensor*{ return new WindSensor(); });
    // Soil
    sensorManager.registerPlugin("soil_moisture", []()->ISensor*{ return new SoilMoistureSensor(); });
    // Distance
    sensorManager.registerPlugin("hcsr04", []()->ISensor*{ return new HCSR04Sensor(); });
    // Temperature (1-Wire)
    sensorManager.registerPlugin("ds18b20", []()->ISensor*{ return new DS18B20Sensor(); });

    // Load sensor configs from /platform_config.json
    if (activeFS) sensorManager.loadAndInit(*activeFS);

    // Register exporters
    exportManager.addExporter(new MqttExporter());
    exportManager.addExporter(new HttpExporter());
    exportManager.addExporter(new SensorCommunityExporter());
    exportManager.addExporter(new OpenSenseMapExporter());
    if (activeFS) exportManager.loadAndInit(*activeFS);

    // Register new API routes (sensor data + config)
    registerApiRoutes(server);

    // Start FreeRTOS task pipeline
    if (activeFS) TaskManager::init(*activeFS);

    Serial.printf("Platform ready. Sensors: %d  Exporters: %d\n",
                  sensorManager.count(), exportManager.count());
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    // ── Early GPIO snapshot (< 1ms) ──────────────────────────────────────────
    // Capture ALL GPIO states BEFORE any delays.
    // Reed switches are momentary; magnet may pass within 50–200 ms.
    {
        earlyGPIO_bitmask = 0;
        for (uint8_t pin = 0; pin <= 10; pin++)
            if (digitalRead(pin)) earlyGPIO_bitmask |= (1UL << pin);
        if (digitalRead(20)) earlyGPIO_bitmask |= (1UL << 20);
        if (digitalRead(21)) earlyGPIO_bitmask |= (1UL << 21);
        earlyGPIO_captured = true;
        earlyGPIO_millis   = millis();
    }

    Serial.begin(115200);
    delay(100);
    Serial.printf("\n\n=== ESP32 Water Logger %s ===\n", getVersionString().c_str());
    Serial.printf("Early GPIO bitmask: 0x%08X\n", earlyGPIO_bitmask);

    loadConfig();

    isrDebounceUs = (unsigned long)config.hardware.debounceMs * 1000UL;

    initStorage();

    int expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;

    // ── Measure button hold duration ─────────────────────────────────────────
    if (earlyGPIO_captured) {
        bool ffStill = (digitalRead(config.hardware.pinWakeupFF) == expectedActive);
        bool pfStill = (digitalRead(config.hardware.pinWakeupPF) == expectedActive);
        if (ffStill || pfStill) {
            unsigned long holdStart = earlyGPIO_millis;
            while (millis() - holdStart < 5000) {
                ffStill = (digitalRead(config.hardware.pinWakeupFF) == expectedActive);
                pfStill = (digitalRead(config.hardware.pinWakeupPF) == expectedActive);
                if (!ffStill && !pfStill) break;
                delay(10);
            }
            buttonHeldMs = millis() - holdStart;
        } else {
            buttonHeldMs = 0;
        }
        Serial.printf("Button held: %lums\n", buttonHeldMs);
    }

    initHardware();   // Configure pin modes AND initialize RTC BEFORE reading time

    // ── Wake timestamp ────────────────────────────────────────────────────────
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
    }

    if (bootcount_restore) { restoreBootCount(); bootcount_restore = false; }
    bootCount++;
    backupBootCount();
    DBGF("Boot count: %d\n", bootCount);

    // ── Wake reason ───────────────────────────────────────────────────────────
    wakeUpButtonStr = getWakeupReason();
    Serial.printf("Wake reason: %s\n", wakeUpButtonStr.c_str());

    int  wifiTrigState  = digitalRead(config.hardware.pinWifiTrigger);

    apModeTriggered = (wifiTrigState == expectedActive) ||
                      (wakeUpButtonStr == "WIFI")       ||
                      config.forceWebServer;

    onlineLoggerMode = config.forceWebServer &&
                       (wifiTrigState != expectedActive) &&
                       (wakeUpButtonStr != "WIFI");

    // ── WiFi + Web Server ─────────────────────────────────────────────────────
    uint8_t platformMode = _detectPlatformMode();

    if (apModeTriggered) {
        Serial.println(onlineLoggerMode ? "=== Online Logger ===" : "=== Web Server ===");
        setCpuFrequencyMhz(160);

        if (!onlineLoggerMode) flushLogBufferToFS();

        if (config.network.wifiMode == WIFIMODE_CLIENT) {
            if (!connectToWiFi()) { wifiFallbackToAP = true; startAPMode(); }
        } else {
            startAPMode();
        }

        setupWebServer();   // ← в WebServer.cpp

        // Platform v5.0: start sensor pipeline in continuous/hybrid mode
        if (platformMode == 1 || platformMode == 2) {
            _initPlatform();
        } else {
            // Even in legacy mode, register API routes so /api/sensors works
            registerApiRoutes(server);
        }

        if (onlineLoggerMode) {
            attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor),
                            onFlowPulse, FALLING);
            configureWakeup();
        }
    } else {
        DBGLN("=== Normal Logging Mode ===");
        setCpuFrequencyMhz(config.hardware.cpuFreqMHz);
        attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor),
                        onFlowPulse, FALLING);

        // Platform v5.0: start sensor pipeline on normal boot when configured
        // for continuous or hybrid mode (no web server routes needed here)
        if (platformMode == 1 || platformMode == 2) {
            _initPlatform();
        }
    }

    // ── Init logging cycle ────────────────────────────────────────────────────
    lastLoggingCycleStartTime = millis();
    cycleStartTime  = millis();
    stateStartTime  = millis();
    lastFlowPulseTime = 0;
    cycleStartedBy  = wakeUpButtonStr.length() > 0 ? wakeUpButtonStr : "BOOT";

    int currentFFState = digitalRead(config.hardware.pinWakeupFF);
    int currentPFState = digitalRead(config.hardware.pinWakeupPF);
    stableFFState    = currentFFState;
    stablePFState    = currentPFState;
    lastFFButtonState = currentFFState;
    lastPFButtonState = currentPFState;
    lastFFDebounceTime = millis();
    lastPFDebounceTime = millis();

    if (wakeUpButtonStr == "FF_BTN") {
        cycleButtonSet = true; cycleStartedBy = "FF_BTN";
        loggingState   = STATE_WAIT_FLOW;
    } else if (wakeUpButtonStr == "PF_BTN") {
        cycleButtonSet = true; cycleStartedBy = "PF_BTN";
        loggingState   = STATE_WAIT_FLOW;
    } else if (onlineLoggerMode) {
        if (currentFFState == expectedActive) {
            cycleStartedBy = "FF_BTN"; cycleButtonSet = true;
            loggingState   = STATE_WAIT_FLOW;
        } else if (currentPFState == expectedActive) {
            cycleStartedBy = "PF_BTN"; cycleButtonSet = true;
            loggingState   = STATE_WAIT_FLOW;
        } else {
            loggingState = STATE_IDLE;
        }
    } else {
        loggingState = STATE_IDLE;
    }

    Serial.println("Setup complete!");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
    // ── Restart check ─────────────────────────────────────────────────────────
    // ПОПРАВКА: използваме safeWiFiShutdown() преди ESP.restart()
    // Това изчиства WiFi radio state и предотвратява "phantom WiFi pin" проблема:
    // при следващ boot earlyGPIO snapshot НЕ вижда стар HIGH на WiFi pin.
    if (shouldRestart && millis() - restartTimer > 2000) {
        Serial.println("Restarting...");
        Serial.flush();
        safeWiFiShutdown();   // ← КЛЮЧОВО: изчиства WiFi преди рестарт
        delay(100);
        ESP.restart();
    }

    // Pure Web Server mode
    if (apModeTriggered && !onlineLoggerMode) {
        delay(10);
        return;
    }

    // ── Button debounce ───────────────────────────────────────────────────────
    debounceButton(config.hardware.pinWakeupFF, lastFFButtonState, stableFFState,
                   lastFFDebounceTime, highCountFF);
    debounceButton(config.hardware.pinWakeupPF, lastPFButtonState, stablePFState,
                   lastPFDebounceTime, highCountPF);

    // Track flow
    if (flowSensorPulseDetected) {
        flowSensorPulseDetected = false;
        lastFlowPulseTime = millis();
    }

    // Test mode LED
    if (config.flowMeter.testMode) {
        static bool pinConfigured = false;
        if (!pinConfigured) { pinMode(config.hardware.pinWifiTrigger, OUTPUT); pinConfigured = true; }
        if (pulseCount > 0 && lastFlowPulseTime > 0) {
            if      (millis() - lastFlowPulseTime < 100)           digitalWrite(config.hardware.pinWifiTrigger, (millis() / config.flowMeter.blinkDuration) % 2);
            else if (millis() - lastFlowPulseTime < TEST_MODE_HOLD_MS) digitalWrite(config.hardware.pinWifiTrigger, HIGH);
            else    digitalWrite(config.hardware.pinWifiTrigger, LOW);
        } else {
            digitalWrite(config.hardware.pinWifiTrigger, LOW);
        }
    }

    // ── State machine ─────────────────────────────────────────────────────────
    switch (loggingState) {

        case STATE_IDLE:
            if (highCountFF > 0) {
                cycleStartedBy = "FF_BTN"; cycleButtonSet = true;
                loggingState   = STATE_WAIT_FLOW;
                stateStartTime = millis(); cycleStartTime = millis();
                if (onlineLoggerMode && Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
                }
                highCountFF = 0; highCountPF = 0;
            } else if (highCountPF > 0) {
                cycleStartedBy = "PF_BTN"; cycleButtonSet = true;
                loggingState   = STATE_WAIT_FLOW;
                stateStartTime = millis(); cycleStartTime = millis();
                if (onlineLoggerMode && Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
                }
                highCountFF = 0; highCountPF = 0;
            } else if (!onlineLoggerMode && g_sleepMode < 2 && !apModeTriggered && millis() - stateStartTime >= 2000) {
                DBGLN("No button -> sleep");
                configureWakeup();
                Serial.flush();
                _doSleep();
            }
            break;

        case STATE_WAIT_FLOW:
            if (pulseCount > 0) {
                loggingState   = STATE_MONITORING;
                stateStartTime = millis();
            } else if (millis() - stateStartTime >= BUTTON_WAIT_FLOW_MS) {
                loggingState = STATE_DONE;
            }
            break;

        case STATE_MONITORING:
            if (millis() - lastFlowPulseTime >= FLOW_IDLE_TIMEOUT_MS) {
                loggingState = STATE_DONE;
            }
            break;

        case STATE_DONE: {
            noInterrupts();
            uint32_t currentPulses = pulseCount;
            interrupts();

            bool hasActivity = (currentPulses > 0 || highCountFF > 0 || highCountPF > 0);

            if (hasActivity) {
                // Post-correction (safe division)
                float ppl = config.flowMeter.pulsesPerLiter;
                if (ppl < 1.0f || !isfinite(ppl)) ppl = 450.0f;
                float cal = config.flowMeter.calibrationMultiplier;
                if (cal <= 0.0f || !isfinite(cal)) cal = 1.0f;
                float corrVol = (float)currentPulses / ppl * cal;
                bool extendedHold = (config.datalog.manualPressThresholdMs > 0) &&
                                    (buttonHeldMs >= config.datalog.manualPressThresholdMs);

                if (config.datalog.postCorrectionEnabled &&
                    highCountFF == 0 && highCountPF == 0 &&
                    corrVol > 0 && !extendedHold) {
                    const char* orig = cycleStartedBy.c_str();
                    bool corrected = false;
                    if (cycleStartedBy == "PF_BTN" && corrVol >= config.datalog.pfToFfThreshold) {
                        cycleStartedBy = "FF_BTN";
                        if (!onlineLoggerMode) wakeUpButtonStr = "FF_BTN";
                        corrected = true;
                    } else if (cycleStartedBy == "FF_BTN" && corrVol <= config.datalog.ffToPfThreshold) {
                        cycleStartedBy = "PF_BTN";
                        if (!onlineLoggerMode) wakeUpButtonStr = "PF_BTN";
                        corrected = true;
                    }
                    if (corrected && fsAvailable && activeFS) {
                        char btnLogPath[80];
                        size_t flen = strlen(config.datalog.folder);
                        if (flen == 0 || (flen == 1 && config.datalog.folder[0] == '/')) {
                            snprintf(btnLogPath, sizeof(btnLogPath), "/btn_log.txt");
                        } else {
                            const char* slash = (config.datalog.folder[flen - 1] == '/') ? "" : "/";
                            const char* lead  = (config.datalog.folder[0] == '/') ? "" : "/";
                            snprintf(btnLogPath, sizeof(btnLogPath), "%s%s%sbtn_log.txt", lead, config.datalog.folder, slash);
                        }
                        File btnLog = activeFS->open(btnLogPath, FILE_APPEND);
                        if (btnLog) {
                            int exp = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
                            bool ffSnap   = earlyGPIO_captured && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWakeupFF)&1));
                            bool pfSnap   = earlyGPIO_captured && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWakeupPF)&1));
                            bool wifiSnap = earlyGPIO_captured && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWifiTrigger)&1));
                            char line[160];
                            snprintf(line, sizeof(line),
                                "#:%d|bitmask:0x%04X|early:FF=%d,PF=%d,WIFI=%d|held:%lums|CORR:%s->%s|L:%.2f",
                                bootCount, earlyGPIO_bitmask,
                                ffSnap, pfSnap, wifiSnap,
                                buttonHeldMs, orig, cycleStartedBy.c_str(), corrVol);
                            btnLog.println(line);
                            btnLog.close();
                        }
                    }
                }

                addLogEntry();
                flushLogBufferToFS();
            }

            if (onlineLoggerMode) {
                noInterrupts(); cycleTotalPulses += pulseCount; pulseCount = 0; interrupts();
                highCountFF = 0; highCountPF = 0;
                cycleStartedBy = "IDLE"; cycleButtonSet = false;
                loggingState   = STATE_IDLE;
                stateStartTime = millis(); cycleStartTime = millis();
                lastFlowPulseTime = 0;
            } else if (!shouldRestart && !onlineLoggerMode && g_sleepMode < 2) {
                configureWakeup();
                DBGLN("Going to sleep...");
                Serial.flush();
                _doSleep();
            }
            break;
        }
    }
}
