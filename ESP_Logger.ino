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
 *
 * ИЗПОЛЗВАНИ БИБЛИОТЕКИ:
 *   Adafruit BME280 Library     (Adafruit)        -> BME280Sensor
 *   Adafruit Unified Sensor     (Adafruit)        -> BME280, BME688
 *   Adafruit BME680 Library     (Adafruit)        -> BME688Sensor
 *   ArduinoJson                 (B. Blanchon)     -> Core
 *   ESPAsyncWebServer           (me-no-dev)       -> WebServer
 *   AsyncTCP                    (me-no-dev)       -> WebServer
 *   PubSubClient                (Nick O'Leary)    -> MqttExporter
 *   OneWire                     (P. Stoffregen)   -> DS18B20Sensor
 *   DallasTemperature           (Miles Burton)    -> DS18B20Sensor
 *   RTC by Makuna               (Makuna)          -> RtcManager
 *
 **************************************************************************************************/

// ============================================================================
// All build-time configuration (module toggles, pin defaults, debug flags,
// task tuning, application timeouts) lives in src/setup.h.
// Edit that single file to change what gets built and how it behaves.
// ============================================================================
#include "src/setup.h"

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_system.h>     // esp_reset_reason()
#include <WiFi.h>           // for WiFi.setSleep() in continuous mode

#include "src/core/Globals.h"
#include "src/core/ModuleRegistry.h"  // Pass 5: unified module registry (phase 1 = empty)
#include "src/modules/WiFiModule.h"    // Pass 5 phase 2
#include "src/modules/OtaModule.h"     // Pass 5 phase 2
#include "src/modules/ThemeModule.h"   // Pass 5 phase 2
#include "src/modules/DataLogModule.h" // Pass 5 phase 2b
#include "src/modules/TimeModule.h"    // Pass 5 phase 2b
#include "src/managers/ConfigManager.h"
#include "src/managers/HardwareManager.h"
#include "src/managers/StorageManager.h"
#include "src/managers/RtcManager.h"
#include "src/managers/WiFiManager.h"
#include "src/managers/DataLogger.h"
#include "src/managers/OtaManager.h"
#include "src/web/WebServer.h"    // setupWebServer()
#include "src/utils/Utils.h"

// ── Platform v5.0 — multi-sensor modules (compiled in only when needed) ──────
#include "src/sensors/SensorManager.h"
// Existing sensors (upgraded) — each guarded by arduino_build_flags.h toggles
#ifdef SENSOR_BME280_ENABLED
#  include "src/sensors/plugins/BME280Sensor.h"
#endif
#ifdef SENSOR_SDS011_ENABLED
#  include "src/sensors/plugins/SDS011Sensor.h"
#endif
#ifdef SENSOR_PMS5003_ENABLED
#  include "src/sensors/plugins/PMS5003Sensor.h"
#endif
#ifdef SENSOR_WATERFLOW_ENABLED
#  include "src/sensors/plugins/WaterFlowSensor.h"   // replaces YFS201Sensor (YF-S201 + YF-S403)
#endif
#ifdef SENSOR_ENS160_ENABLED
#  include "src/sensors/plugins/ENS160Sensor.h"
#endif
#ifdef SENSOR_SGP30_ENABLED
#  include "src/sensors/plugins/SGP30Sensor.h"
#endif
#ifdef SENSOR_RAIN_ENABLED
#  include "src/sensors/plugins/RainSensor.h"
#endif
#ifdef SENSOR_WIND_ENABLED
#  include "src/sensors/plugins/WindSensor.h"
#endif
// New sensors
#ifdef SENSOR_VEML6075_ENABLED
#  include "src/sensors/plugins/VEML6075Sensor.h"
#endif
#ifdef SENSOR_VEML7700_ENABLED
#  include "src/sensors/plugins/VEML7700Sensor.h"
#endif
#ifdef SENSOR_BH1750_ENABLED
#  include "src/sensors/plugins/BH1750Sensor.h"
#endif
#ifdef SENSOR_SOIL_ENABLED
#  include "src/sensors/plugins/SoilMoistureSensor.h"
#endif
#ifdef SENSOR_SCD4X_ENABLED
#  include "src/sensors/plugins/SCD4xSensor.h"
#endif
#ifdef SENSOR_BME688_ENABLED
#  include "src/sensors/plugins/BME688Sensor.h"
#endif
#ifdef SENSOR_HCSR04_ENABLED
#  include "src/sensors/plugins/HCSR04Sensor.h"
#endif
#ifdef SENSOR_DS18B20_ENABLED
#  include "src/sensors/plugins/DS18B20Sensor.h"
#endif
#ifdef SENSOR_ZMPT101B_ENABLED
#  include "src/sensors/plugins/ZMPT101BSensor.h"
#endif
#ifdef SENSOR_ZMCT103C_ENABLED
#  include "src/sensors/plugins/ZMCT103CSensor.h"
#endif
#include "src/pipeline/DataPipeline.h"
#include "src/tasks/TaskManager.h"
#include "src/export/ExportManager.h"
#ifdef EXPORT_MQTT_ENABLED
#  include "src/export/MqttExporter.h"
#endif
#ifdef EXPORT_HTTP_ENABLED
#  include "src/export/HttpExporter.h"
#endif
#ifdef EXPORT_SENSORCOMMUNITY_ENABLED
#  include "src/export/SensorCommunityExporter.h"
#endif
#ifdef EXPORT_OPENSENSEMAP_ENABLED
#  include "src/export/OpenSenseMapExporter.h"
#endif
#ifdef EXPORT_WEBHOOK_ENABLED
#  include "src/export/WebhookExporter.h"
#endif
#include "src/web/ApiHandlers.h"

// ============================================================================
// PLATFORM MODE & SLEEP GLOBALS
// ============================================================================
// Active platform mode (set once in setup, read in loop). See PlatformMode in Config.h.
static PlatformMode g_platformMode  = PLATFORM_LEGACY;
#ifdef EXPORT_MQTT_ENABLED
MqttExporter* g_mqttExporter = nullptr;  // for HA discovery + API access (external linkage — referenced from web/ApiHandlers.cpp)
#endif

// Continuous-mode idle power management
static uint32_t g_contIdleMs       = 300000; // ms idle before reducing power (5 min)
static uint8_t  g_contIdleCpuMhz   = 80;     // CPU MHz when idle (vs 160 when active)
static bool     g_contModemSleep   = true;   // enable WiFi modem sleep when idle
static uint32_t g_contLastActivity = 0;      // millis() of last tracked activity
static bool     g_contPowerReduced = false;  // true once idle power applied

// Hybrid-mode deep sleep (periodic timer wake for sensor readings)
static uint32_t g_hybridIdleMs    = 120000;  // ms idle in STATE_IDLE → deep sleep
static uint32_t g_hybridSleepMs   = 60000;   // deep sleep duration (ms)
static uint32_t g_hybridActiveMs  = 30000;   // active window on timer wake (ms)
static uint32_t g_hybridIdleStart = 0;       // millis() when STATE_IDLE began

// ============================================================================
// _doSleep() — graceful deep sleep
// Shuts down FreeRTOS tasks for continuous/hybrid before sleeping.
// ============================================================================
static void _doSleep() {
    // Confirm any pending OTA before sleeping.  Without this, a fresh OTA
    // install in legacy mode (which sleeps within ~2 s of wake) would never
    // reach the 90 s tick() deadline before the device reset, and the
    // bootloader would roll a perfectly healthy image back on the next
    // wake (codex review PR #49).  confirm() is idempotent — no-op when
    // already confirmed or no pending verify.
    OtaManager::confirm();

    // Give platform tasks a chance to finish current work
    if (g_platformMode != PLATFORM_LEGACY && TaskManager::running) {
        TaskManager::shutdown();
        delay(200);
    }
    DBGLN("[Sleep] Deep sleep →");
    Serial.flush();
    delay(10);
    esp_deep_sleep_start();
}

// ============================================================================
// _loadSleepConfig() — read sleep settings from platform_config.json
// ============================================================================
static void _loadSleepConfig() {
    if (!fsAvailable || !activeFS) return;
    File f = activeFS->open("/platform_config.json", FILE_READ);
    if (!f) return;
    // Use a dedicated small doc — sleep section only needs ~256 bytes
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    JsonObjectConst sl = doc["sleep"];
    if (sl.isNull()) return;

    JsonObjectConst cont = sl["continuous"];
    if (!cont.isNull()) {
        g_contIdleMs     = cont["idle_timeout_ms"] | (uint32_t)300000;
        g_contIdleCpuMhz = (uint8_t)(cont["idle_cpu_mhz"] | 80);
        g_contModemSleep = cont["modem_sleep"]      | true;
    }

    JsonObjectConst hyb = sl["hybrid"];
    if (!hyb.isNull()) {
        g_hybridIdleMs   = hyb["idle_before_sleep_ms"] | (uint32_t)120000;
        g_hybridSleepMs  = hyb["sleep_duration_ms"]    | (uint32_t)60000;
        g_hybridActiveMs = hyb["active_window_ms"]     | (uint32_t)30000;
    }
}

// ============================================================================
// _manageContinuousPower() — reduce CPU + enable modem sleep after idle
// Called every loop() iteration in continuous mode.
// ============================================================================
static void _manageContinuousPower() {
    // Reset activity clock on explicit external events
    if (g_contLastActivity == 0) g_contLastActivity = millis(); // init once

    // C2: web server activity restores full power
    if (g_lastWebActivity > g_contLastActivity) {
        g_contLastActivity = g_lastWebActivity;
        if (g_contPowerReduced) {
            setCpuFrequencyMhz(160);
            WiFi.setSleep(false);
            g_contPowerReduced = false;
            DBGLN("[Sleep] Power restored (web activity)");
        }
    }

    uint32_t idleMs = millis() - g_contLastActivity;

    if (!g_contPowerReduced && idleMs >= g_contIdleMs) {
        // Throttle CPU and enable WiFi modem sleep
        setCpuFrequencyMhz(g_contIdleCpuMhz);
        if (g_contModemSleep) WiFi.setSleep(true);
        DBGF("[Sleep] Continuous idle %us → CPU=%uMHz modem_sleep=%d\n",
                      idleMs / 1000, g_contIdleCpuMhz, (int)g_contModemSleep);
        g_contPowerReduced = true;
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Detect operating mode from /platform_config.json
// ---------------------------------------------------------------------------
static PlatformMode _detectPlatformMode() {
    if (!fsAvailable || !activeFS) return PLATFORM_LEGACY;
    File f = activeFS->open("/platform_config.json", FILE_READ);
    if (!f) return PLATFORM_LEGACY;
    // Use a filter so only the top-level "mode" key is parsed; the rest of
    // platform_config.json (sensors array, etc.) can be many KB and would
    // overflow a small document, returning NoMemory and silently falling back
    // to legacy mode even when continuous/hybrid is configured.
    StaticJsonDocument<8>   filter;
    filter["mode"] = true;
    StaticJsonDocument<64>  doc;
    if (deserializeJson(doc, f, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
        f.close(); return PLATFORM_LEGACY;
    }
    f.close();
    const char* mode = doc["mode"] | "legacy";
    if (strcmp(mode, "continuous") == 0) return PLATFORM_CONTINUOUS;
    if (strcmp(mode, "hybrid")     == 0) return PLATFORM_HYBRID;
    return PLATFORM_LEGACY;
}

// ---------------------------------------------------------------------------
// Watchdog recovery log — write reset reason to /reset_log.txt on LittleFS
// Appends one line per boot so resets can be diagnosed via the Files page.
// ---------------------------------------------------------------------------
static const char* _resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "POWER_ON";
        case ESP_RST_EXT:      return "EXT_RESET";
        case ESP_RST_SW:       return "SW_RESET";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

static void _writeResetLog() {
    if (!activeFS) return;
    esp_reset_reason_t reason = esp_reset_reason();
    // Only log notable resets (skip normal power-on and deep-sleep wake)
    if (reason == ESP_RST_POWERON || reason == ESP_RST_DEEPSLEEP) return;

    File f = activeFS->open("/reset_log.txt", FILE_APPEND);
    if (!f) return;

    // Format: YYYY-MM-DD HH:MM:SS  REASON  boot#N
    // RTC time may not be valid at this point; use uptime placeholder
    char line[80];
    snprintf(line, sizeof(line), "boot#%u  reason=%s\n",
             (unsigned)bootCount, _resetReasonStr(reason));
    f.print(line);
    f.close();
    DBGF("[WDT] Reset log entry: %s", line);
}

// ---------------------------------------------------------------------------
// M9/2.6 — Check for sensor pin conflicts with hardware config pins
// ---------------------------------------------------------------------------
static void _checkPinConflicts() {
    if (!activeFS) return;
    File f = activeFS->open("/platform_config.json", FILE_READ);
    if (!f) return;
    StaticJsonDocument<4096> doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
    f.close();

    JsonArray sensors = doc["sensors"].as<JsonArray>();
    if (sensors.isNull()) return;

    int flowPin = config.hardware.pinFlowSensor;
    for (JsonObject s : sensors) {
        if (!s["enabled"]) continue;
        int sPin = s["pin"] | -1;
        if (sPin >= 0 && sPin == flowPin) {
            DBGF("[WARN] M9: Sensor '%s' (type=%s) pin %d conflicts with "
                          "pinFlowSensor — dual ISR registration will panic!\n",
                          (const char*)(s["id"] | s["type"] | "?"),
                          (const char*)(s["type"] | "?"), flowPin);
        }
    }
}

// ---------------------------------------------------------------------------
// Register all sensor plugins and init pipeline (called in continuous/hybrid)
// ---------------------------------------------------------------------------
static void _initPlatform() {
    DBGLN("=== Platform v5.0: initialising sensors ===");

    // Register all plugins (guarded by arduino_build_flags.h toggles)
#ifdef SENSOR_BME280_ENABLED
    sensorManager.registerPlugin("bme280",  []()->ISensor*{ return new BME280Sensor(); });
    sensorManager.registerPlugin("bmp280",  []()->ISensor*{ return new BME280Sensor(); });
#endif
#ifdef SENSOR_BME688_ENABLED
    sensorManager.registerPlugin("bme688",  []()->ISensor*{ return new BME688Sensor(); });
#endif
#ifdef SENSOR_SDS011_ENABLED
    sensorManager.registerPlugin("sds011",  []()->ISensor*{ return new SDS011Sensor(); });
#endif
#ifdef SENSOR_PMS5003_ENABLED
    sensorManager.registerPlugin("pms5003", []()->ISensor*{ return new PMS5003Sensor(); });
#endif
#ifdef SENSOR_ENS160_ENABLED
    sensorManager.registerPlugin("ens160",  []()->ISensor*{ return new ENS160Sensor(); });
#endif
#ifdef SENSOR_SGP30_ENABLED
    sensorManager.registerPlugin("sgp30",   []()->ISensor*{ return new SGP30Sensor(); });
#endif
#ifdef SENSOR_SCD4X_ENABLED
    sensorManager.registerPlugin("scd4x",   []()->ISensor*{ return new SCD4xSensor(); });
#endif
#ifdef SENSOR_VEML6075_ENABLED
    sensorManager.registerPlugin("veml6075",[]()->ISensor*{ return new VEML6075Sensor(); });
#endif
#ifdef SENSOR_VEML7700_ENABLED
    sensorManager.registerPlugin("veml7700",[]()->ISensor*{ return new VEML7700Sensor(); });
#endif
#ifdef SENSOR_BH1750_ENABLED
    sensorManager.registerPlugin("bh1750",  []()->ISensor*{ return new BH1750Sensor(); });
#endif
#ifdef SENSOR_WATERFLOW_ENABLED
    sensorManager.registerPlugin("yfs201",      []()->ISensor*{ return new WaterFlowSensor("yfs201",      450.0f); });
    sensorManager.registerPlugin("yfs403",      []()->ISensor*{ return new WaterFlowSensor("yfs403",      600.0f); });
    sensorManager.registerPlugin("water_flow",  []()->ISensor*{ return new WaterFlowSensor("water_flow",  0.0f);   });
#endif
#ifdef SENSOR_RAIN_ENABLED
    sensorManager.registerPlugin("rain",    []()->ISensor*{ return new RainSensor(); });
#endif
#ifdef SENSOR_WIND_ENABLED
    sensorManager.registerPlugin("wind",    []()->ISensor*{ return new WindSensor(); });
#endif
#ifdef SENSOR_SOIL_ENABLED
    sensorManager.registerPlugin("soil_moisture", []()->ISensor*{ return new SoilMoistureSensor(); });
#endif
#ifdef SENSOR_HCSR04_ENABLED
    sensorManager.registerPlugin("hcsr04", []()->ISensor*{ return new HCSR04Sensor(); });
#endif
#ifdef SENSOR_DS18B20_ENABLED
    sensorManager.registerPlugin("ds18b20", []()->ISensor*{ return new DS18B20Sensor(); });
#endif
#ifdef SENSOR_ZMPT101B_ENABLED
    sensorManager.registerPlugin("zmpt101b", []()->ISensor*{ return new ZMPT101BSensor(); });
#endif
#ifdef SENSOR_ZMCT103C_ENABLED
    sensorManager.registerPlugin("zmct103c", []()->ISensor*{ return new ZMCT103CSensor(); });
#endif

    // Load sensor configs from /platform_config.json
    if (activeFS) sensorManager.loadAndInit(*activeFS);

    // Detect sensor pin conflicts with hardware flow sensor pin (M9)
    _checkPinConflicts();

    // Register exporters (guarded by arduino_build_flags.h toggles)
#ifdef EXPORT_MQTT_ENABLED
    g_mqttExporter = new MqttExporter();
    exportManager.addExporter(g_mqttExporter);
#endif
#ifdef EXPORT_HTTP_ENABLED
    exportManager.addExporter(new HttpExporter());
#endif
#ifdef EXPORT_SENSORCOMMUNITY_ENABLED
    exportManager.addExporter(new SensorCommunityExporter());
#endif
#ifdef EXPORT_OPENSENSEMAP_ENABLED
    exportManager.addExporter(new OpenSenseMapExporter());
#endif
#ifdef EXPORT_WEBHOOK_ENABLED
    exportManager.addExporter(new WebhookExporter());
#endif
    if (activeFS) exportManager.loadAndInit(*activeFS);
    // Spool failed exports to LittleFS (always available, even without SD) (#4.7)
    if (littleFsAvailable) exportManager.setSpoolFS(&LittleFS);

    // Publish HA MQTT discovery payloads (runs after sensors and exporters are loaded)
#ifdef EXPORT_MQTT_ENABLED
    if (g_mqttExporter) g_mqttExporter->publishHaDiscovery();
#endif

    // Register new API routes (sensor data + config)
    registerApiRoutes(server);

    // Start FreeRTOS task pipeline
    if (activeFS) TaskManager::init(*activeFS);

    DBGF("Platform ready. Sensors: %d  Exporters: %d\n",
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
        // L1: include pins 18-21 (SuperMini exposes 18-19)
        for (uint8_t pin = 18; pin <= 21; pin++)
            if (digitalRead(pin)) earlyGPIO_bitmask |= (1UL << pin);
        earlyGPIO_captured = true;
        earlyGPIO_millis   = millis();
    }

    Serial.begin(115200);
    delay(100);
    DBGF("\n\n=== ESP32 Water Logger %s ===\n", getVersionString().c_str());
    DBGF("Early GPIO bitmask: 0x%08X\n", earlyGPIO_bitmask);

    loadConfig();

    isrDebounceUs = (uint32_t)config.hardware.debounceMs * 1000UL;   // I1

    initStorage();

    // Pass 5 phase 2: register IModule adapters for WiFi/OTA/theme and
    // hydrate them from /config/modules.json.  config.bin still wins when
    // the file is missing (first boot); saveConfig() will seed it on the
    // next save_* so subsequent boots read from JSON.
    moduleRegistry.add(&WiFiModule::instance());
    moduleRegistry.add(&OtaModule::instance());
    moduleRegistry.add(&ThemeModule::instance());
    moduleRegistry.add(&DataLogModule::instance());
    moduleRegistry.add(&TimeModule::instance());
    if (fsAvailable && activeFS) {
        moduleRegistry.loadAll(*activeFS);
        if (!activeFS->exists(ModuleRegistry::DEFAULT_PATH)) {
            moduleRegistry.saveAll(*activeFS);  // seed from current DeviceConfig
        }
    }

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
        DBGF("Button held: %lums\n", buttonHeldMs);
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

    // ── Watchdog recovery log ─────────────────────────────────────────────────
    _writeResetLog();

    // ── OTA rollback support ─────────────────────────────────────────────────
    OtaManager::boot();

    // ── Wake reason ───────────────────────────────────────────────────────────
    wakeUpButtonStr = getWakeupReason();
    DBGF("Wake reason: %s\n", wakeUpButtonStr.c_str());

    int  wifiTrigState  = digitalRead(config.hardware.pinWifiTrigger);

    apModeTriggered = (wifiTrigState == expectedActive) ||
                      (wakeUpButtonStr == "WIFI")       ||
                      config.forceWebServer;

    onlineLoggerMode = config.forceWebServer &&
                       (wifiTrigState != expectedActive) &&
                       (wakeUpButtonStr != "WIFI");

    // ── WiFi + Web Server ─────────────────────────────────────────────────────
    g_platformMode = _detectPlatformMode();

    // Set sleep guard: continuous keeps FreeRTOS tasks alive → no legacy deep sleep
    // Hybrid also blocks legacy 2-second sleep; its own timed sleep runs in loop()
    g_sleepMode = (g_platformMode != PLATFORM_LEGACY) ? 2 : 0;

    // Load per-mode sleep tuning from platform_config.json
    _loadSleepConfig();

    // In hybrid/continuous mode the web server (and WiFi) always starts — no button press required.
    // Exception: hybrid timer wake keeps its headless sensor cycle so WiFi doesn't delay readings.
    // If wifiMode=CLIENT and connection fails, the existing fallback below starts AP mode.
    if (g_platformMode != PLATFORM_LEGACY && !apModeTriggered) {
        bool isHybridTimerWake = (g_platformMode == PLATFORM_HYBRID && wakeUpButtonStr == "TIMER");
        if (!isHybridTimerWake) {
            apModeTriggered = true;
            DBGLN("Non-legacy: auto-activating web server");
        }
    }

    if (apModeTriggered) {
        DBGLN(onlineLoggerMode ? "=== Online Logger ===" : "=== Web Server ===");
        setCpuFrequencyMhz(160);

        if (!onlineLoggerMode) flushLogBufferToFS();

        if (config.network.wifiMode == WIFIMODE_CLIENT) {
            if (!connectToWiFi()) { wifiFallbackToAP = true; startAPMode(); }
        } else {
            startAPMode();
        }

        setupWebServer();   // ← в WebServer.cpp

        // Platform v5.0: start sensor pipeline in continuous/hybrid mode
        if (g_platformMode != PLATFORM_LEGACY) {
            _initPlatform();
        } else {
            // Even in legacy mode, register API routes so /api/sensors works
            registerApiRoutes(server);
        }

        if (onlineLoggerMode) {
            // Skip Arduino attachInterrupt when platform mode is active:
            // WaterFlowSensor::init() already registers the ISR via
            // gpio_isr_handler_add(); dual registration causes a panic (#6).
            if (g_platformMode == PLATFORM_LEGACY) {
                attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor),
                                onFlowPulse, FALLING);
            }
            configureWakeup();
        }
    } else {
        DBGLN("=== Normal Logging Mode ===");
        setCpuFrequencyMhz(config.hardware.cpuFreqMHz);
        // Guard: skip Arduino ISR when WaterFlowSensor is handling the pin (#6)
        if (g_platformMode == PLATFORM_LEGACY) {
            attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor),
                            onFlowPulse, FALLING);
        }

        // Platform v5.0: start sensor pipeline on normal boot when configured
        // for continuous or hybrid mode (no web server routes needed here)
        if (g_platformMode != PLATFORM_LEGACY) {
            _initPlatform();
        }
    }

    // ── Hybrid timer wakeup: quick headless sensor cycle → back to sleep ─────
    // When the hybrid periodic timer fires, skip the full web/flow cycle:
    // just let sensor tasks run for g_hybridActiveMs, flush data, then re-sleep.
    if (g_platformMode == PLATFORM_HYBRID && !apModeTriggered && wakeUpButtonStr == "TIMER") {
        DBGF("[Hybrid] Timer wake: sensor window %ums...\n", g_hybridActiveMs);
        // H2: non-blocking active window — poll in 100ms steps so shouldRestart is honoured
        uint32_t windowEnd = millis() + g_hybridActiveMs;
        while (millis() < windowEnd) {
            if (shouldRestart) { OtaManager::confirm(); safeWiFiShutdown(); delay(100); ESP.restart(); }
            delay(100);
        }
        flushLogBufferToFS();
        configureWakeup();            // re-arm GPIO wakeup sources
        esp_sleep_enable_timer_wakeup((uint64_t)g_hybridSleepMs * 1000ULL);
        TaskManager::shutdown();
        DBGF("[Hybrid] Timer cycle done — sleeping %us\n", g_hybridSleepMs / 1000);
        Serial.flush();
        // Confirm pending OTA before deep-sleep — same reason as _doSleep().
        OtaManager::confirm();
        esp_deep_sleep_start();
        // unreachable — execution resumes from setup() after wakeup
    }

    // ── Continuous mode: init activity clock ─────────────────────────────────
    if (g_platformMode == PLATFORM_CONTINUOUS) {
        g_contLastActivity = millis();
        // Enable WiFi modem sleep from the start if configured
        if (g_contModemSleep) WiFi.setSleep(true);
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

    // In platform mode (>=1), WaterFlowSensor owns the flow ISR; the legacy
    // flow state machine has no pulse source and must stay in STATE_IDLE.
    if (g_platformMode != PLATFORM_LEGACY) {
        loggingState = STATE_IDLE;
    } else if (wakeUpButtonStr == "FF_BTN") {
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

    DBGLN("Setup complete!");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
    // ── OTA rollback watchdog ────────────────────────────────────────────────
    // No-op once the running image is confirmed; while pending, confirms
    // automatically after OTA_CONFIRM_TIMEOUT_MS (default 90 s) of stable
    // operation.  A panic or hardware-watchdog reset before then triggers
    // a bootloader-level rollback to the previous slot.
    OtaManager::tick(millis());
    // ── Captive-portal DNS pump (no-op when AP mode is off) ───────────────────
    // The DNS responder is non-blocking; we just need to drain the queue once
    // per loop iteration so phones get prompt replies and the OS-level portal
    // banner triggers within a second of joining the AP.
    tickCaptivePortalDNS();

    // ── Deferred NTP sync ─────────────────────────────────────────────────────
    // The /sync_time web handler sets g_pendingNtpSync=1 and returns 202 so it
    // doesn't block the AsyncTCP worker. We run the actual sync (up to ~10 s)
    // here on the main task, where blocking is fine.
    if (g_pendingNtpSync == 1) {
        g_pendingNtpSync = 2;  // running
        bool ok = syncTimeFromNTP();
        if (ok) rtcValid = true;
        g_lastNtpSyncResult = ok ? 1 : -1;
        g_pendingNtpSync    = 0;
    }

    // ── SSE live heartbeat (1 Hz) ─────────────────────────────────────────────
    // No-op when no EventSource clients are subscribed.
    {
        static uint32_t s_lastLiveTick = 0;
        uint32_t now = millis();
        if (now - s_lastLiveTick >= 1000) {
            s_lastLiveTick = now;
            publishLiveEvent();
        }
    }

    // ── Restart check ─────────────────────────────────────────────────────────
    // ПОПРАВКА: използваме safeWiFiShutdown() преди ESP.restart()
    // Това изчиства WiFi radio state и предотвратява "phantom WiFi pin" проблема:
    // при следващ boot earlyGPIO snapshot НЕ вижда стар HIGH на WiFi pin.
    if (shouldRestart && millis() - restartTimer > 2000) {
        DBGLN("Restarting...");
        Serial.flush();
        // Confirm pending OTA before deliberate reboot — see _doSleep().
        OtaManager::confirm();
        safeWiFiShutdown();   // ← КЛЮЧОВО: изчиства WiFi преди рестарт
        delay(100);
        ESP.restart();
    }

    // ── Continuous platform mode ──────────────────────────────────────────────
    // FreeRTOS tasks handle all sensing; loop() only manages idle power.
    // C3: buttons remain functional — debounce + publish events through pipeline.
    if (g_platformMode == PLATFORM_CONTINUOUS) {
        _manageContinuousPower();

        // C3: poll buttons in continuous mode
        debounceButton(config.hardware.pinWakeupFF, lastFFButtonState, stableFFState,
                       lastFFDebounceTime, highCountFF);
        debounceButton(config.hardware.pinWakeupPF, lastPFButtonState, stablePFState,
                       lastPFDebounceTime, highCountPF);

        if (highCountFF > 0 || highCountPF > 0) {
            // Publish button event as SensorReading through the pipeline
            uint32_t ts = 0;
            if (Rtc) { RtcDateTime now = Rtc->GetDateTime(); if (now.IsValid()) ts = now.Unix32Time(); }
            if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

            if (highCountFF > 0) {
                SensorReading btn = SensorReading::make(ts, "buttons", "gpio",
                    "ff_press", (float)highCountFF, "count", QUALITY_GOOD);
                if (sensorQueue) xQueueSend(sensorQueue, &btn, 0);
                highCountFF = 0;
            }
            if (highCountPF > 0) {
                SensorReading btn = SensorReading::make(ts, "buttons", "gpio",
                    "pf_press", (float)highCountPF, "count", QUALITY_GOOD);
                if (sensorQueue) xQueueSend(sensorQueue, &btn, 0);
                highCountPF = 0;
            }
            // Reset idle timer — user is active
            g_contLastActivity = millis();
            g_lastWebActivity  = millis();
        }

        // C4: software watchdog
        if (!TaskManager::checkHealth()) {
            shouldRestart = true; restartTimer = millis();
        }
        delay(10);
        return;
    }

    // Pure Web Server mode (legacy or hybrid with web server active)
    if (apModeTriggered && !onlineLoggerMode) {
        // Hybrid: still manage power if idle
        if (g_platformMode == PLATFORM_HYBRID) _manageContinuousPower();
        // C4: software watchdog in hybrid/web mode
        if (g_platformMode != PLATFORM_LEGACY && !TaskManager::checkHealth()) {
            shouldRestart = true; restartTimer = millis();
        }
        delay(10);
        // Hybrid must fall through to the idle-sleep check at the bottom of loop().
        if (g_platformMode != PLATFORM_HYBRID) return;
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
            if      (millis() - lastFlowPulseTime < 100 && config.flowMeter.blinkDuration > 0) digitalWrite(config.hardware.pinWifiTrigger, (millis() / config.flowMeter.blinkDuration) % 2);
            else if (millis() - lastFlowPulseTime < TEST_MODE_HOLD_MS) digitalWrite(config.hardware.pinWifiTrigger, HIGH);
            else    digitalWrite(config.hardware.pinWifiTrigger, LOW);
        } else {
            digitalWrite(config.hardware.pinWifiTrigger, LOW);
        }
    }

    // ── State machine ─────────────────────────────────────────────────────────
    switch (loggingState) {

        case STATE_IDLE:
            if (g_platformMode == PLATFORM_LEGACY && highCountFF > 0) {
                cycleStartedBy = "FF_BTN"; cycleButtonSet = true;
                loggingState   = STATE_WAIT_FLOW;
                stateStartTime = millis(); cycleStartTime = millis();
                if (onlineLoggerMode && Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
                }
                highCountFF = 0; highCountPF = 0;
            } else if (g_platformMode == PLATFORM_LEGACY && highCountPF > 0) {
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
            // L2: single atomic read+clear — eliminates race between here and addLogEntry
            noInterrupts();
            uint32_t currentPulses = pulseCount;
            pulseCount = 0;
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
                            bool ffSnap   = earlyGPIO_captured && config.hardware.pinWakeupFF < 32   && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWakeupFF)&1));
                            bool pfSnap   = earlyGPIO_captured && config.hardware.pinWakeupPF < 32   && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWakeupPF)&1));
                            bool wifiSnap = earlyGPIO_captured && config.hardware.pinWifiTrigger < 32 && ((exp==HIGH)==(bool)((earlyGPIO_bitmask>>config.hardware.pinWifiTrigger)&1));
                            char line[160];
                            snprintf(line, sizeof(line),
                                "#:%d|bitmask:0x%08lX|early:FF=%d,PF=%d,WIFI=%d|held:%lums|CORR:%s->%s|L:%.2f",
                                bootCount, earlyGPIO_bitmask,
                                ffSnap, pfSnap, wifiSnap,
                                buttonHeldMs, orig, cycleStartedBy.c_str(), corrVol);
                            btnLog.println(line);
                            btnLog.close();
                        }
                    }
                }

                addLogEntry(currentPulses);   // L2: pass captured pulses
                flushLogBufferToFS();
            }

            if (onlineLoggerMode) {
                noInterrupts(); cycleTotalPulses += currentPulses; interrupts();  // L2: already cleared above
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
            } else if (!shouldRestart && !onlineLoggerMode && g_platformMode == PLATFORM_HYBRID) {
                // Hybrid: after a flow cycle, reset idle timer so hybrid sleep
                // won't fire immediately — give sensors time to log the event.
                g_hybridIdleStart = millis();
            }
            break;
        }
    }

    // ── Hybrid mode: deep sleep after extended idle ───────────────────────────
    // g_sleepMode == 2 blocks the legacy 2-second sleep above.
    // Instead, wait g_hybridIdleMs in STATE_IDLE before sleeping with both
    // a GPIO wakeup (buttons/flow) and a periodic timer for sensor reads.
    if (g_platformMode == PLATFORM_HYBRID && !onlineLoggerMode && !shouldRestart) {
        // H4: also allow sleep when web server is active but no clients connected
        bool webIdle = apModeTriggered ? (WiFi.softAPgetStationNum() == 0) : true;
        if (loggingState == STATE_IDLE && webIdle) {
            if (g_hybridIdleStart == 0) g_hybridIdleStart = millis();

            if (millis() - g_hybridIdleStart >= g_hybridIdleMs) {
                flushLogBufferToFS();
                configureWakeup(); // GPIO: buttons + flow pin
                esp_sleep_enable_timer_wakeup((uint64_t)g_hybridSleepMs * 1000ULL);
                DBGF("[Hybrid] Idle %us → deep sleep %us (GPIO+timer)\n",
                              g_hybridIdleMs / 1000, g_hybridSleepMs / 1000);
                // H3: clean WiFi before sleep if it was started
                if (wifiConnectedAsClient || apModeTriggered) safeWiFiShutdown();
                _doSleep();
            }
        } else {
            // Active cycle in progress or clients connected — reset idle timer
            g_hybridIdleStart = 0;
        }
    }
}
