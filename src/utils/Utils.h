#pragma once
#include <Arduino.h>
#include <FS.h>
#include "../core/Config.h"

// ---- String helpers ----
String formatFileSize(uint64_t bytes);
String getVersionString();

// ---- Path helpers ----
String buildPath(const String& dir, const String& name);
String sanitizePath(const String& path);
String sanitizeFilename(const String& filename);

// ---- FS helpers ----
bool deleteRecursive(fs::FS& fs, const String& path);
