// ============================================================================
// setup.h — central build-time configuration for ESP32 Water Logger v5.0
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
#ifndef SENSOR_BME688_ENABLED
#  define SENSOR_BME688_ENABLED       // BME680/BME688 (I2C)
#endif
#ifndef SENSOR_DS18B20_ENABLED
#  define SENSOR_DS18B20_ENABLED      // DS18B20 (1-Wire)
#endif
#ifndef SENSOR_SDS011_ENABLED
#  define SENSOR_SDS011_ENABLED       // SDS011 PM2.5/PM10 (UART)
#endif
#ifndef SENSOR_PMS5003_ENABLED
#  define SENSOR_PMS5003_ENABLED      // PMS5003 PM (UART)
#endif
#ifndef SENSOR_ENS160_ENABLED
#  define SENSOR_ENS160_ENABLED       // ENS160 air quality (I2C)
#endif
#ifndef SENSOR_SGP30_ENABLED
#  define SENSOR_SGP30_ENABLED        // SGP30 VOC (I2C)
#endif
#ifndef SENSOR_SCD4X_ENABLED
#  define SENSOR_SCD4X_ENABLED        // SCD40/SCD41 CO2 (I2C)
#endif
#ifndef SENSOR_VEML6075_ENABLED
#  define SENSOR_VEML6075_ENABLED     // VEML6075 UV (I2C)
#endif
#ifndef SENSOR_VEML7700_ENABLED
#  define SENSOR_VEML7700_ENABLED     // VEML7700 lux (I2C)
#endif
#ifndef SENSOR_BH1750_ENABLED
#  define SENSOR_BH1750_ENABLED       // BH1750 lux (I2C)
#endif
#ifndef SENSOR_WATERFLOW_ENABLED
#  define SENSOR_WATERFLOW_ENABLED    // YF-S201 / YF-S403 water flow (GPIO)
#endif
#ifndef SENSOR_RAIN_ENABLED
#  define SENSOR_RAIN_ENABLED         // Tipping-bucket rain gauge (GPIO)
#endif
#ifndef SENSOR_WIND_ENABLED
#  define SENSOR_WIND_ENABLED         // Anemometer + wind vane (GPIO/ADC)
#endif
#ifndef SENSOR_SOIL_ENABLED
#  define SENSOR_SOIL_ENABLED         // Capacitive soil moisture (ADC)
#endif
#ifndef SENSOR_HCSR04_ENABLED
#  define SENSOR_HCSR04_ENABLED       // HC-SR04 ultrasonic distance (GPIO)
#endif
#ifndef SENSOR_ZMPT101B_ENABLED
#  define SENSOR_ZMPT101B_ENABLED     // ZMPT101B AC voltage (ADC)
#endif
#ifndef SENSOR_ZMCT103C_ENABLED
#  define SENSOR_ZMCT103C_ENABLED     // ZMCT103C AC current (ADC)
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
#  define DEFAULT_SDA        6
#endif
#ifndef DEFAULT_SCL
#  define DEFAULT_SCL        7
#endif
#ifndef DEFAULT_FLOW_PIN
#  define DEFAULT_FLOW_PIN  21
#endif

// ============================================================================
// 3. DEBUG / BUILD FLAGS
// ============================================================================
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
#ifndef STACK_STORAGE_TASK
#  define STACK_STORAGE_TASK     6144   // Two JsonLogger + File I/O
#endif
#ifndef STACK_EXPORT_TASK
#  define STACK_EXPORT_TASK      8192   // WiFi + TLS + JSON serialisation
#endif

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

// JsonLogger write buffer (lines flushed at once)
#ifndef LOG_BUF_LINES
#  define LOG_BUF_LINES 8
#endif
