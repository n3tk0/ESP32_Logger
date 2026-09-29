#pragma once
// An enum field from a module config, as a number or as a numeric string.
//
// The Settings › Modules form sends a <select>'s value, which pages older
// than the one that types it send as a string ("1"). `cfg[k] | def` returns
// def for that, so the change was dropped while the save answered ok. The
// web pages live on LittleFS and the firmware updates without them, so the
// firmware accepts both. Anything else keeps def.
#include <ArduinoJson.h>
#include <stdlib.h>

inline int jsonEnumInt(JsonVariantConst v, int def) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        char* end = nullptr;
        long n = strtol(s, &end, 10);
        if (end != s && *end == '\0') return (int)n;
    }
    return def;
}
