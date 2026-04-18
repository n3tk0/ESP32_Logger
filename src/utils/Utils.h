#pragma once
#include <Arduino.h>
#include <FS.h>
#include "../core/Config.h"

// getVersionString() is defined inline in Config.h – do NOT redeclare here.

// ---- String helpers ----
String formatFileSize(uint64_t bytes);

// ---- Path helpers ----
String buildPath(const String& dir, const String& name);

// Normalise a path for FS access.
// Returns an empty String ("") if the input is unsafe (contains "..", control
// characters, backslash, or NUL). Callers MUST check isEmpty() and reject.
// Safe inputs produce a leading-slash, no duplicate slashes, no trailing slash
// (except root "/").
String sanitizePath(const String& path);

// Validate a single filename component (no slashes).
// Returns "" on unsafe input (empty, too long, contains "/" "\" or control
// chars, or is "." / "..").  Callers MUST check isEmpty().
String sanitizeFilename(const String& filename);

// True if `path` resolves to a firmware-critical file that UI/API must never
// modify or delete (config blob, boot-count, reset log, recovery bundle).
bool isPathProtected(const String& path);

// ---- FS helpers ----
bool deleteRecursive(fs::FS& fs, const String& path);