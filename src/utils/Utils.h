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

// ---- URL helpers ----
// Percent-encode a string per RFC 3986 unreserved set (A-Z a-z 0-9 -_.~).
// Used by the /api/v1/* alias layer to rebuild query strings from parsed
// params — this fork of ESPAsyncWebServer-esphome doesn't expose the raw
// queryString(), so values returned by AsyncWebParameter::value() (which
// are url-decoded) need re-encoding before they go back into a Location
// header.  Centralised here so future call sites don't reinvent it.
String urlEncode(const String& v);