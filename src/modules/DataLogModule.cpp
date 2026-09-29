#include "DataLogModule.h"
#include "ModuleSchemas.h"      // this module's form, gzipped
#include "../core/Globals.h"
#include "../core/Config.h"
#include <stdlib.h>
#include <string.h>

namespace {

void copyStr(char* dst, size_t n, const char* src) {
    if (src) strlcpy(dst, src, n);
}

// An enum field as a number. The module form has posted a <select>'s value
// as a string ("2"), and web files on LittleFS can be older than the
// firmware, so both spellings count; anything else keeps `def`.
int enumOr(JsonVariantConst v, int def) {
    if (v.is<int>()) return v.as<int>();
    const char* s = v.as<const char*>();
    return (s && *s >= '0' && *s <= '9') ? atoi(s) : def;
}

} // namespace

// ---------------------------------------------------------------------------
// Single source of truth for "load JSON -> config.datalog" — used by both
// /save_datalog (form -> JsonDocument -> here) and /import_settings (file
// -> JsonObject -> here). PR #105 follow-up: clamp ranges that previously
// lived only in the HTTP handler are mirrored here so the JSON path
// applies the same bounds.  Path-traversal validation stays at the HTTP
// boundary — load() trusts that prefix/folder/currentFile have already
// been sanitized (no slashes in prefix, sanitizePath()'d folder /
// currentFile).
bool datalogPrefixOk(const char* s, size_t n) {
    if (!s || n == 0 || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;   // NUL included
    }
    return !(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.');
}

// ---------------------------------------------------------------------------
bool DataLogModule::load(JsonObjectConst cfg) {
    DatalogConfig& d = config.datalog;
    copyStr(d.prefix,       sizeof(d.prefix),       cfg["prefix"]      | (const char*)nullptr);
    copyStr(d.folder,       sizeof(d.folder),       cfg["folder"]      | (const char*)nullptr);
    copyStr(d.currentFile,  sizeof(d.currentFile),  cfg["currentFile"] | (const char*)nullptr);

    d.rotation              = (DatalogRotation)enumOr(cfg["rotation"], (int)d.rotation);
    if ((int)d.rotation < 0 || (int)d.rotation > 4) d.rotation = (DatalogRotation)0;
    if (cfg["maxSizeKB"].is<int>()) {
        int v = cfg["maxSizeKB"].as<int>();
        d.maxSizeKB  = (v < 10) ? 10 : (v > 10000 ? 10000 : (uint32_t)v);
    }
    if (cfg["maxEntries"].is<int>()) {
        int v = cfg["maxEntries"].as<int>();
        d.maxEntries = (v < 10) ? 10 : (v > 65535 ? 65535 : (uint16_t)v);
    }
    d.timestampFilename     = cfg["timestampFilename"]   | d.timestampFilename;
    d.includeDeviceId       = cfg["includeDeviceId"]     | d.includeDeviceId;
    d.includeBootCount      = cfg["includeBootCount"]    | d.includeBootCount;
    d.includeExtraPresses   = cfg["includeExtraPresses"] | d.includeExtraPresses;
    d.dateFormat            = (uint8_t)enumOr(cfg["dateFormat"], (int)d.dateFormat);
    if (d.dateFormat > 4) d.dateFormat = 0;
    d.timeFormat            = (uint8_t)enumOr(cfg["timeFormat"], (int)d.timeFormat);
    if (d.timeFormat > 2) d.timeFormat = 0;
    d.endFormat             = (uint8_t)enumOr(cfg["endFormat"], (int)d.endFormat);
    if (d.endFormat > 2) d.endFormat = 0;
    d.volumeFormat          = (uint8_t)enumOr(cfg["volumeFormat"], (int)d.volumeFormat);
    if (d.volumeFormat > 3) d.volumeFormat = 0;
    if (cfg["manualPressThresholdMs"].is<int>()) {
        int v = cfg["manualPressThresholdMs"].as<int>();
        d.manualPressThresholdMs = (v < 0) ? 0 : (v > 60000 ? 60000 : (uint16_t)v);
    }
    d.postCorrectionEnabled = cfg["postCorrectionEnabled"]  | d.postCorrectionEnabled;
    // By value, here and in save(): DeviceConfig is packed, these fields sit
    // at odd offsets, and ArduinoJson takes its operands by reference — a
    // reference to a misaligned float, which the compiler then loads as if it
    // were aligned (UBSan reports it in tests/host/test_settings_json.cpp).
    d.pfToFfThreshold       = cfg["pfToFfThreshold"] | (float)d.pfToFfThreshold;
    if (!(d.pfToFfThreshold >= 0.1f && d.pfToFfThreshold <= 1000.0f)) d.pfToFfThreshold = 4.5f;
    d.ffToPfThreshold       = cfg["ffToPfThreshold"] | (float)d.ffToPfThreshold;
    if (!(d.ffToPfThreshold >= 0.1f && d.ffToPfThreshold <= 1000.0f)) d.ffToPfThreshold = 3.7f;
    return true;
}

// ---------------------------------------------------------------------------
bool DataLogModule::save(JsonObject cfg) const {
    const DatalogConfig& d = config.datalog;
    cfg["prefix"]                 = d.prefix;
    cfg["folder"]                 = d.folder;
    cfg["rotation"]               = (int)d.rotation;
    cfg["maxSizeKB"]              = (uint32_t)d.maxSizeKB;   // by value: see load()
    cfg["maxEntries"]             = (uint16_t)d.maxEntries;
    cfg["timestampFilename"]      = d.timestampFilename;
    cfg["includeDeviceId"]        = d.includeDeviceId;
    cfg["includeBootCount"]       = d.includeBootCount;
    cfg["includeExtraPresses"]    = d.includeExtraPresses;
    cfg["dateFormat"]             = (int)d.dateFormat;
    cfg["timeFormat"]             = (int)d.timeFormat;
    cfg["endFormat"]              = (int)d.endFormat;
    cfg["volumeFormat"]           = (int)d.volumeFormat;
    cfg["manualPressThresholdMs"] = (uint16_t)d.manualPressThresholdMs;
    cfg["postCorrectionEnabled"]  = d.postCorrectionEnabled;
    cfg["pfToFfThreshold"]        = (float)d.pfToFfThreshold;
    cfg["ffToPfThreshold"]        = (float)d.ffToPfThreshold;
    return true;
}

// ---------------------------------------------------------------------------
ModuleSchema DataLogModule::schema() const {
    return {MODULE_SCHEMA_DATALOG_GZ, sizeof(MODULE_SCHEMA_DATALOG_GZ)};
}

// ---------------------------------------------------------------------------
// Live status chip — a compact summary of the rotation policy + retention.
// Reads config only (no FS scan), so it is safe on the AsyncTCP worker.
void DataLogModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;                       // UI shows "disabled"
    static const char* const ROT[] = { "no rotation", "daily", "weekly", "monthly", "by size" };
    const DatalogConfig& d = config.datalog;
    int r = (int)d.rotation;
    if (r < 0 || r > 4) r = 0;
    String t = ROT[r];
    if (r == 4) { t += " ("; t += String(d.maxSizeKB); t += " KB)"; }
    else        { t += " \xC2\xB7 "; t += String(d.maxEntries); t += " rows"; }
    out["text"] = t;
    out["tone"] = "ok";
}
