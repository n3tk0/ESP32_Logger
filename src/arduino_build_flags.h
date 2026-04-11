// ============================================================================
// arduino_build_flags.h — DEPRECATED compatibility shim
// ============================================================================
// All build-time configuration has moved to src/setup.h.  This file is kept
// only so older sketches / forks that still `#include "src/arduino_build_flags.h"`
// continue to compile.  Please include `src/setup.h` directly instead.
//
// Arduino IDE setup (XIAO ESP32-C3):
//   Board:     XIAO ESP32-C3  (via Boards Manager → "esp32 by Espressif")
//   Partition: Huge APP (3MB No OTA/1MB SPIFFS)  ← REQUIRED for full build
//              Or use "Minimal SPIFFS" + disable unused sensors in setup.h
//   Upload:    921600 baud
//   Monitor:   115200 baud
//
// Required libraries (Tools → Manage Libraries):
//   - ArduinoJson          (bblanchon)             >= 7.0.0
//   - ESPAsyncWebServer    (esphome / lacamera)    >= 3.1.0
//   - AsyncTCP             (me-no-dev)             >= 1.1.1
//
// LittleFS data upload (for web UI & platform_config.json):
//   Install the "arduino-esp32fs-plugin" or use Arduino IDE 2.x with the
//   ESP32 Sketch Data Upload plugin.  Place files from www/ into /data.
// ============================================================================

#pragma once

#include "setup.h"
