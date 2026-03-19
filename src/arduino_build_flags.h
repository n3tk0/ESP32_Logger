// ============================================================================
// arduino_build_flags.h — Arduino IDE compatibility shim
// ============================================================================
// When building with Arduino IDE (not PlatformIO), this file replaces the
// build_flags that platformio.ini would normally inject at compile time.
//
// Arduino IDE setup:
//   Board:     XIAO ESP32-C3  (via Boards Manager → "esp32 by Espressif")
//   Partition: Minimal SPIFFS  (Tools → Partition Scheme)
//   Upload:    921600 baud
//   Monitor:   115200 baud
//
// Required libraries (install via Tools → Manage Libraries):
//   - ArduinoJson          (bblanchon)             >= 7.0.0
//   - ESPAsyncWebServer    (esphome / lacamera)     >= 3.1.0
//   - AsyncTCP             (me-no-dev)              >= 1.1.1
//   - PubSubClient         (Nick O'Leary)           >= 2.8.0
//   - Adafruit BME280      (Adafruit)               >= 2.2.4
//   - Adafruit BME680      (Adafruit)               >= 2.0.4
//   - Adafruit Unified Sensor (Adafruit)            >= 1.1.14
//   - OneWire              (Paul Stoffregen)        >= 2.3.8
//   - DallasTemperature    (Miles Burton)           >= 3.11.0
//   - Rtc                  (Makuna)                 >= 2.4.2
//
// LittleFS data upload (for web UI & platform_config.json):
//   Install the "arduino-esp32fs-plugin" or use Arduino IDE 2.x with the
//   ESP32 Sketch Data Upload plugin.
//   Place files from www/ into the sketch's /data directory.
// ============================================================================

#pragma once

// ── FreeRTOS: single-core for ESP32-C3 ─────────────────────────────────────
#ifndef CONFIG_FREERTOS_UNICORE
#  define CONFIG_FREERTOS_UNICORE 1
#endif

// ── Debug verbosity (0 = off, match platformio.ini defaults) ───────────────
#ifndef CORE_DEBUG_LEVEL
#  define CORE_DEBUG_LEVEL 0
#endif
#ifndef DEBUG_MODE
#  define DEBUG_MODE 0
#endif

// ── Default hardware pins for XIAO ESP32-C3 ────────────────────────────────
// These match the platformio.ini -D flags and act as fallback values when
// pin numbers are not overridden by the config file.
#ifndef DEFAULT_SDA
#  define DEFAULT_SDA        6
#endif
#ifndef DEFAULT_SCL
#  define DEFAULT_SCL        7
#endif
#ifndef DEFAULT_FLOW_PIN
#  define DEFAULT_FLOW_PIN  21
#endif
