// ============================================================================
// setup.h — central build-time configuration for ESP32 Water Logger v5.1.0
// ============================================================================
// This is the single place to configure WHAT GETS BUILT and HOW IT BEHAVES.
//
//   • Module toggles  — enable/disable sensors and exporters
//   • GPIO defaults   — fallback pin numbers when not in platform_config.json
//   • Debug flags     — Serial output verbosity, FreeRTOS unicore
//   • Task tuning     — FreeRTOS priorities, stack sizes, queue depths
//   • Timing/limits   — log batch sizes, timeouts, intervals
//
// All macros use #ifndef so PlatformIO -D build_flags can override them
// without editing this file.  Arduino IDE users can edit this file directly.
//
// Supported targets:
//   • XIAO ESP32-C3        (SDA=6, SCL=7)
//   • ESP32-C3 Super Mini  (SDA=8, SCL=9, USB CDC On Boot)
//   • Generic ESP32
//
// What is NOT here:
//   • Data structures (DeviceConfig, HardwareConfig, ...) — see core/Config.h
//   • Enums (PlatformMode, StorageType, ...)              — see core/Config.h
//   • Default file paths / SSIDs / NTP server             — see core/Config.h
//   • Version numbers                                     — see core/Config.h
// ============================================================================

#pragma once

// ============================================================================
// 1. MODULE TOGGLES — comment out to exclude a sensor or exporter from build
// ============================================================================
// Reduces flash usage (~5-20 KB per module).  Only enable what you actually
// have wired up.  PlatformIO build_flags override these via -D defines.
// ----------------------------------------------------------------------------
// Sensors with internal mini drivers (no external library needed)
#ifndef SENSOR_BME280_ENABLED
#  define SENSOR_BME280_ENABLED       // BME280/BMP280 (I2C)
#endif
//#ifndef SENSOR_BME688_ENABLED
//#  define SENSOR_BME688_ENABLED       // BME680/BME688 (I2C)
//#endif
//#ifndef SENSOR_DS18B20_ENABLED
//#  define SENSOR_DS18B20_ENABLED      // DS18B20 (1-Wire)
//#endif
#ifndef SENSOR_SDS011_ENABLED
#  define SENSOR_SDS011_ENABLED       // SDS011 PM2.5/PM10 (UART)
#endif
//#ifndef SENSOR_PMS5003_ENABLED
//#  define SENSOR_PMS5003_ENABLED      // PMS5003 PM (UART)
//#endif
//#ifndef SENSOR_SPS30_ENABLED
//#  define SENSOR_SPS30_ENABLED        // Sensirion SPS30 PM1/2.5/4/10 (I2C)
//#endif
//#ifndef SENSOR_ENS160_ENABLED
//#  define SENSOR_ENS160_ENABLED       // ENS160 air quality (I2C)
//#endif
//#ifndef SENSOR_SGP30_ENABLED
//#  define SENSOR_SGP30_ENABLED        // SGP30 VOC (I2C)
//#endif
//#ifndef SENSOR_SCD4X_ENABLED
//#  define SENSOR_SCD4X_ENABLED        // SCD40/SCD41 CO2 (I2C)
//#endif
//#ifndef SENSOR_VEML6075_ENABLED
//#  define SENSOR_VEML6075_ENABLED     // VEML6075 UV (I2C)
//#endif
//#ifndef SENSOR_VEML7700_ENABLED
//#  define SENSOR_VEML7700_ENABLED     // VEML7700 lux (I2C)
//#endif
//#ifndef SENSOR_BH1750_ENABLED
//#  define SENSOR_BH1750_ENABLED       // BH1750 lux (I2C)
//#endif
//#ifndef SENSOR_WATERFLOW_ENABLED
//#  define SENSOR_WATERFLOW_ENABLED    // YF-S201 / YF-S403 water flow (GPIO)
//#endif
//#ifndef SENSOR_RAIN_ENABLED
//#  define SENSOR_RAIN_ENABLED         // Tipping-bucket rain gauge (GPIO)
//#endif
//#ifndef SENSOR_WIND_ENABLED
//#  define SENSOR_WIND_ENABLED         // Anemometer + wind vane (GPIO/ADC)
//#endif
//#ifndef SENSOR_SOIL_ENABLED
//#  define SENSOR_SOIL_ENABLED         // Capacitive soil moisture (ADC)
//#endif
//#ifndef SENSOR_HCSR04_ENABLED
//#  define SENSOR_HCSR04_ENABLED       // HC-SR04 ultrasonic distance (GPIO)
//#endif
//#ifndef SENSOR_ZMPT101B_ENABLED
//#  define SENSOR_ZMPT101B_ENABLED     // ZMPT101B AC voltage (ADC)
//#endif
//#ifndef SENSOR_ZMCT103C_ENABLED
//#  define SENSOR_ZMCT103C_ENABLED     // ZMCT103C AC current (ADC)
//#endif

// Actuators.  OFF by default and opt-in for a reason: this one drives a
// MOSFET gate from a PWM pin.  Enabling it only compiles the module in — it
// stays disabled and unattached until a pin is assigned in the UI.
//#ifndef MODULE_HEATER_ENABLED
//#  define MODULE_HEATER_ENABLED       // Enclosure heater (frost/condensation)
//#endif

// Remote sensor nodes.  Compiles in POST /api/ingest and the "remote" sensor
// plugin, so satellite boards (see node/ for the ESP8266 reference client)
// can push readings into this device's pipeline.  A remote sensor is then
// configured like any other, with "type":"remote".
//
// CHANGE THE TOKEN before putting this on a network you share.  It is the
// only thing between the pipeline and anything else that can reach port 80:
//   -DFEATURE_REMOTE_NODES -DINGEST_TOKEN='"your-token-here"'
//#ifndef FEATURE_REMOTE_NODES
//#  define FEATURE_REMOTE_NODES        // POST /api/ingest + "remote" sensor
//#endif

// Battery nodes over ESP-NOW.  A second kind of remote node: deep-sleeping,
// battery powered, no WiFi association and no HTTP.  Readings land in the same
// RemoteIngest mailbox as POST /api/ingest, so everything downstream — filters,
// ring buffer, exporters, dashboard — is shared.  See docs/ESPNOW_NODE.md.
//
// CHANGE THE KEY before putting this on a network you share.  It is a single
// 16-byte secret shared by every node, and it is what both encrypts the link
// and authorises a node to be adopted during pairing:
//   -DFEATURE_ESPNOW_INGEST -DESPNOW_LMK='"16-byte-secret!!"'
//
// TWO CONSEQUENCES OF TURNING THIS ON, both deliberate:
//
//   • FEATURE_REMOTE_NODES is implied.  RemoteIngest is where the readings go
//     and RemoteNodeSensor is what drains them; without both there is nowhere
//     for an ESP-NOW reading to land.  (POST /api/ingest comes with it.  If
//     that endpoint is unwanted on this device, set a long INGEST_TOKEN — it
//     is refused without one.)
//
//   • WiFi modem sleep is forced OFF.  WIFI_PS_MIN_MODEM breaks ESP-NOW
//     unicast — measured — while leaving broadcast working, so pairing would
//     succeed and then no reading would ever arrive.  The sketch asks
//     modemSleepAllowed() at each point of use rather than overriding the
//     config once — a device with no sleep settings at all never reaches the
//     parser that would have applied the override, which is how the first
//     version of this reached exactly the state it existed to prevent.
//     A mains-powered collector loses nothing by it.
//#ifndef FEATURE_ESPNOW_INGEST
//#  define FEATURE_ESPNOW_INGEST       // ESP-NOW receive path + pairing
//#endif

#ifdef FEATURE_ESPNOW_INGEST
#  ifndef FEATURE_REMOTE_NODES
#    define FEATURE_REMOTE_NODES
#  endif
#endif

// Kindle dashboard — GET /kindle.  A server-rendered, JavaScript-free page
// sized for a 6" e-ink reader's browser.  See docs/KINDLE_DASHBOARD.md.
//#ifndef FEATURE_KINDLE_DASHBOARD
//#  define FEATURE_KINDLE_DASHBOARD   // GET /kindle e-ink dashboard
//#endif

// Weather forecast client.  Fetches a short forecast over HTTPS for the
// Kindle dashboard to sit alongside the measured values.
//#ifndef MODULE_FORECAST_ENABLED
//#  define MODULE_FORECAST_ENABLED    // HTTPS weather forecast
//#endif

// SD card storage.  ON by default, because turning it off changes what a
// device with a card fitted can do.
//
// Comment the define out to drop `#include <SD.h>` and, with it, the FatFs
// library and SD driver underneath it.  MEASURED at ~34 KB on the C3 — two
// `pio run -e xiao_esp32c3` builds of this default config: firmware.bin is
// 1,333,408 bytes with and 1,298,832 without, so 34,576 off the image that
// has to fit app0 (0x170000 = 1,507,328).  It is spent whether or not a card
// is ever fitted.
//
// With this off: sdFs() returns nullptr and sdAvailable stays false, so every
// request naming storage=sdcard falls back to LittleFS — the same fallback a
// card-capable build already takes when no card is fitted, so no call site
// changes.  StorageManager says so once on the serial log at boot, and
// /api/diag reports storage.sd_supported=false so the UI can tell "no card"
// from "no driver".  See src/core/SdCompat.h.
#ifndef FEATURE_SD_STORAGE
#  define FEATURE_SD_STORAGE
#endif

// Cloud / network exporters
#ifndef EXPORT_MQTT_ENABLED
#  define EXPORT_MQTT_ENABLED            // Internal MQTT driver
#endif
#ifndef EXPORT_HTTP_ENABLED
#  define EXPORT_HTTP_ENABLED            // Generic HTTP POST
#endif
#ifndef EXPORT_SENSORCOMMUNITY_ENABLED
#  define EXPORT_SENSORCOMMUNITY_ENABLED // sensor.community
#endif
#ifndef EXPORT_OPENSENSEMAP_ENABLED
#  define EXPORT_OPENSENSEMAP_ENABLED    // openSenseMap
#endif
#ifndef EXPORT_WEBHOOK_ENABLED
#  define EXPORT_WEBHOOK_ENABLED         // Generic webhook (Discord/Slack/IFTTT)
#endif

// ============================================================================
// 2. GPIO DEFAULT PINS — fallback values used when not in platform_config.json
// ============================================================================
// PlatformIO -D flags override these per board.  Arduino IDE users edit here.
// Defaults below match the XIAO ESP32-C3 wiring.
// ----------------------------------------------------------------------------
#ifndef DEFAULT_SDA
#  define DEFAULT_SDA        8
#endif
#ifndef DEFAULT_SCL
#  define DEFAULT_SCL        9
#endif
// Second I2C controller. Only reachable on parts with SOC_I2C_NUM == 2
// (ESP32-S3, classic ESP32) — the ESP32-C3 has a single controller and a
// sensor configured for bus 1 there is refused at init with an explicit log
// line. Pins are per-sensor config ("bus", "sda", "scl" in
// platform_config.json); these are only the fallbacks offered by the UI.
// A second bus needs its own pull-ups, not just its own pins.
#ifndef DEFAULT_SDA1
#  define DEFAULT_SDA1      10
#endif
#ifndef DEFAULT_SCL1
#  define DEFAULT_SCL1      11
#endif
#ifndef DEFAULT_FLOW_PIN
#  define DEFAULT_FLOW_PIN  4
#endif

// ============================================================================
// 3. DEBUG / BUILD FLAGS
// ============================================================================
// PLATFORM_LEGACY_BUILD — when 1 (default) the legacy flowmeter run logger
// (deepsleep / wake-on-button cycles, RTC RAM bootcount, FF/PF press
// recording, pipe-delimited TXT output) is compiled into the firmware.
// Set to 0 (-DPLATFORM_LEGACY_BUILD=0) for non-legacy deployments to drop
// the entire legacy code path; PLATFORM_HYBRID gets per-run flowmeter
// logging via FlowRunLogger instead, and PLATFORM_CONTINUOUS streams flow
// readings into the wide-CSV pipeline like any other sensor.
#ifndef PLATFORM_LEGACY_BUILD
#  define PLATFORM_LEGACY_BUILD 1
#endif

// FreeRTOS unicore: only ESP32-C3 / C6 are single-core.
// Dual-core chips (ESP32, ESP32-S3) MUST NOT define CONFIG_FREERTOS_UNICORE.
#ifndef CONFIG_FREERTOS_UNICORE
#  if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#    define CONFIG_FREERTOS_UNICORE 1
#  endif
#endif

// ESP-IDF/Arduino core log verbosity (0 = none, 5 = verbose)
#ifndef CORE_DEBUG_LEVEL
#  define CORE_DEBUG_LEVEL 0
#endif

// Application Serial debug output (DBG/DBGLN/DBGF macros in Config.h).
// 0 saves ~3 KB flash.
#ifndef DEBUG_MODE
#  define DEBUG_MODE 0
#endif

// ============================================================================
// 4. FREERTOS TASK TUNING
// ============================================================================
// Higher number = higher priority.  Sensor task has top priority so timing-
// sensitive reads (I2C, 1-Wire) aren't preempted.
#ifndef TASK_PRIO_SENSOR
#  define TASK_PRIO_SENSOR      3
#endif
#ifndef TASK_PRIO_PROCESS
#  define TASK_PRIO_PROCESS     2
#endif
#ifndef TASK_PRIO_SLOW_SENSOR
#  define TASK_PRIO_SLOW_SENSOR 2   // Blocking sensors — same as process
#endif
#ifndef TASK_PRIO_STORAGE
#  define TASK_PRIO_STORAGE     1
#endif
#ifndef TASK_PRIO_EXPORT
#  define TASK_PRIO_EXPORT      1
#endif

// Stack sizes in bytes (tuned for ESP32-C3, 400 KB DRAM total)
#ifndef STACK_SENSOR_TASK
#  define STACK_SENSOR_TASK      4096
#endif
#ifndef STACK_PROCESS_TASK
#  define STACK_PROCESS_TASK     6144   // LTTB intermediate buffer on stack
#endif
#ifndef STACK_SLOW_SENSOR_TASK
#  define STACK_SLOW_SENSOR_TASK 4096   // Blocking sensor reads (UART + delay)
#endif
// SlowSensorTask outer poll cadence.  Override to reduce duty-cycle for
// sensors whose internal measurement window is >> 500 ms.  (AUDIT 10.4)
#ifndef SLOW_SENSOR_TICK_MS
#  define SLOW_SENSOR_TICK_MS    500
#endif
#ifndef STACK_STORAGE_TASK
#  define STACK_STORAGE_TASK     8192   // LiveAggregator (~2.4 KB) + StorageTask
                                        // local row/header buffers (2 KB) +
                                        // CsvLogger.appendRow() 1 KB on-stack
                                        // existing-header buffer + FS driver
                                        // overhead — needs headroom for
                                        // worst-case wide-CSV schema.
#endif
#ifndef STACK_EXPORT_TASK
#  define STACK_EXPORT_TASK      8192   // WiFi + TLS + JSON serialisation
#endif

// R28 / AUDIT 5.5: compile-time DRAM budget guard.  ESP32-C3 has 400 KB SRAM
// total; the task stacks below are allocated from DRAM at task creation.
// 50 KB is the headroom ceiling we want to keep for heap (JsonDocument,
// AsyncWebServer connections, sensor buffers).  Tripping this assert means
// someone bumped a stack — re-budget before merging.
static_assert(
    STACK_SENSOR_TASK + STACK_PROCESS_TASK + STACK_SLOW_SENSOR_TASK +
    STACK_STORAGE_TASK + STACK_EXPORT_TASK < 50000,
    "Sum of task stack sizes exceeds the 50 KB DRAM budget for ESP32-C3. "
    "Reduce one of STACK_SENSOR_TASK / STACK_PROCESS_TASK / "
    "STACK_SLOW_SENSOR_TASK / STACK_STORAGE_TASK / STACK_EXPORT_TASK.");

// Queue depths (items = SensorReading, ~80 bytes each)
#ifndef QUEUE_SENSOR_DEPTH
#  define QUEUE_SENSOR_DEPTH  20
#endif
#ifndef QUEUE_STORAGE_DEPTH
#  define QUEUE_STORAGE_DEPTH 32
#endif
#ifndef QUEUE_EXPORT_DEPTH
#  define QUEUE_EXPORT_DEPTH  32
#endif

// ============================================================================
// 5. APPLICATION TUNING — timeouts, batch sizes, intervals
// ============================================================================
// Logger / state machine
#ifndef LOG_BATCH_SIZE
#  define LOG_BATCH_SIZE 16            // RTC log buffer slots (deep-sleep safe)
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#  define WIFI_CONNECT_TIMEOUT_MS 15000
#endif
#ifndef TEST_MODE_BLINK_MS
#  define TEST_MODE_BLINK_MS 250
#endif
#ifndef TEST_MODE_HOLD_MS
#  define TEST_MODE_HOLD_MS 1000
#endif
#ifndef ISR_DEBOUNCE_MICROS
#  define ISR_DEBOUNCE_MICROS 1000     // 1 ms flow-pulse debounce
#endif
#ifndef BUTTON_WAIT_FLOW_MS
#  define BUTTON_WAIT_FLOW_MS 6000
#endif
#ifndef FLOW_IDLE_TIMEOUT_MS
#  define FLOW_IDLE_TIMEOUT_MS 3000
#endif

// OTA — auto-confirmation timeout for new firmware (must boot cleanly first)
#ifndef OTA_CONFIRM_TIMEOUT_MS
#  define OTA_CONFIRM_TIMEOUT_MS 90000  // 90 s
#endif

// Web HTTP Basic Auth — opt-in at compile time.
// When WEB_BASIC_AUTH_ENABLED=1, every HTTP request requires the given
// credentials. Off by default to preserve local-LAN ergonomics; enable for
// internet-exposed deployments. Override via -DWEB_BASIC_AUTH_ENABLED=1 etc.
#ifndef WEB_BASIC_AUTH_ENABLED
#  define WEB_BASIC_AUTH_ENABLED 0
#endif
#ifndef WEB_BASIC_AUTH_USER
#  define WEB_BASIC_AUTH_USER "admin"
#endif
#ifndef WEB_BASIC_AUTH_PASS
#  define WEB_BASIC_AUTH_PASS "admin"
#endif

// Export pipeline batching (ExportTask)
#ifndef EXPORT_BATCH_SIZE
#  define EXPORT_BATCH_SIZE 20
#endif
#ifndef EXPORT_FLUSH_INTERVAL_MS
#  define EXPORT_FLUSH_INTERVAL_MS 60000  // 1 min max wait
#endif

// Spool drain batching (ExportManager)
#ifndef EXPORT_SPOOL_BATCH
#  define EXPORT_SPOOL_BATCH 20
#endif
#ifndef EXPORT_MAX_SENDALL_MS
#  define EXPORT_MAX_SENDALL_MS 30000     // 30 s circuit breaker
#endif

