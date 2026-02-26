/**************************************************************************************************
 * PROJECT: ESP32 Low-Power Water Usage Logger v4.1.4
 * TARGET:  XIAO ESP32-C3 (RISC-V)
 * AUTHOR:  Petko Georgiev / Villeroy & Boch Bulgaria
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
    initStorage();

    // ── Measure button hold duration ─────────────────────────────────────────
    if (earlyGPIO_captured) {
        int expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
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
            buttonHeldMs = millis() - earlyGPIO_millis;
        }
        Serial.printf("Button held: %lums\n", buttonHeldMs);
    }

    // ── Wake timestamp ────────────────────────────────────────────────────────
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
    }

    if (bootcount_restore) { restoreBootCount(); bootcount_restore = false; }
    bootCount++;
    backupBootCount();
    DBGF("Boot count: %d\n", bootCount);

    initHardware();   // Configure pin modes BEFORE reading fallback states

    // ── Wake reason ───────────────────────────────────────────────────────────
    wakeUpButtonStr = getWakeupReason();
    Serial.printf("Wake reason: %s\n", wakeUpButtonStr.c_str());

    int  expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
    int  wifiTrigState  = digitalRead(config.hardware.pinWifiTrigger);

    apModeTriggered = (wifiTrigState == expectedActive) ||
                      (wakeUpButtonStr == "WIFI")       ||
                      config.forceWebServer;

    onlineLoggerMode = config.forceWebServer &&
                       (wifiTrigState != expectedActive) &&
                       (wakeUpButtonStr != "WIFI");

    // ── WiFi + Web Server ─────────────────────────────────────────────────────
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
            } else if (!onlineLoggerMode && !apModeTriggered && millis() - stateStartTime >= 2000) {
                DBGLN("No button -> sleep");
                configureWakeup();
                Serial.flush();
                esp_deep_sleep_start();
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
                // Post-correction
                float corrVol = (float)currentPulses
                                / config.flowMeter.pulsesPerLiter
                                * config.flowMeter.calibrationMultiplier;
                bool extendedHold = (config.datalog.manualPressThresholdMs > 0) &&
                                    (buttonHeldMs >= config.datalog.manualPressThresholdMs);

                if (config.datalog.postCorrectionEnabled &&
                    highCountFF == 0 && highCountPF == 0 &&
                    corrVol > 0 && !extendedHold) {
                    String orig = cycleStartedBy;
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
                        String folder = String(config.datalog.folder);
                        if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
                        if (folder.length() > 0 && !folder.endsWith("/"))   folder += "/";
                        if (folder.isEmpty()) folder = "/";
                        String btnLogPath = folder + "btn_log.txt";
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
                                buttonHeldMs, orig.c_str(), cycleStartedBy.c_str(), corrVol);
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
            } else if (!shouldRestart) {
                configureWakeup();
                DBGLN("Deep sleep...");
                Serial.flush();
                esp_deep_sleep_start();
            }
            break;
        }
    }
}
