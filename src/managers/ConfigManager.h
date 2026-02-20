#pragma once
#include <Arduino.h>

void loadDefaultConfig();
bool loadConfig();
bool saveConfig();
void migrateConfig(uint8_t fromVersion);
String generateDeviceId();
void regenerateDeviceId();
