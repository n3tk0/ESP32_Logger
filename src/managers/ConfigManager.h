#pragma once
#include <Arduino.h>

void loadDefaultConfig();
bool loadConfig();
bool saveConfig();
void migrateConfig(uint8_t fromVersion);
String generateDeviceId();
void regenerateDeviceId();

// Fill any zero/empty fields in the in-memory config with safe defaults.
// Idempotent — skips fields that already hold non-default values.  Called
// after every load path, and also from /export_settings to guarantee the
// payload is complete even if config somehow ends up in a mid-state.
void fillConfigDefaults();
