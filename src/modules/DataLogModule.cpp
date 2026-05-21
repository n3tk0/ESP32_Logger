#include "DataLogModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"

namespace {

void copyStr(char* dst, size_t n, const char* src) {
    if (src) strlcpy(dst, src, n);
}

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char DATALOG_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"prefix\",\"type\":\"string\",\"max\":32,\"label\":\"Filename prefix\"},"
      "{\"id\":\"folder\",\"type\":\"string\",\"max\":32,\"label\":\"Folder\"},"
      "{\"id\":\"rotation\",\"type\":\"enum\",\"label\":\"Rotation\","
        "\"options\":[{\"v\":0,\"l\":\"None\"},{\"v\":1,\"l\":\"Daily\"},"
                     "{\"v\":2,\"l\":\"Weekly\"},{\"v\":3,\"l\":\"Monthly\"},"
                     "{\"v\":4,\"l\":\"By size\"}]},"
      "{\"id\":\"maxSizeKB\",\"type\":\"int\",\"min\":0,\"max\":1048576,\"label\":\"Max size (KB)\","
        "\"showIf\":{\"rotation\":4}},"
      "{\"id\":\"maxEntries\",\"type\":\"int\",\"min\":10,\"max\":65535,\"label\":\"Max entries\"},"
      "{\"id\":\"timestampFilename\",\"type\":\"bool\",\"label\":\"Timestamp in filename\"},"
      "{\"id\":\"includeDeviceId\",\"type\":\"bool\",\"label\":\"Include device ID\"},"
      "{\"id\":\"includeBootCount\",\"type\":\"bool\",\"label\":\"Include boot count\"},"
      "{\"id\":\"includeExtraPresses\",\"type\":\"bool\",\"label\":\"Log extra presses\"},"
      "{\"id\":\"dateFormat\",\"type\":\"enum\",\"label\":\"Date format\","
        "\"options\":[{\"v\":0,\"l\":\"Off\"},{\"v\":1,\"l\":\"DD/MM/YYYY\"},"
                     "{\"v\":2,\"l\":\"MM/DD/YYYY\"},{\"v\":3,\"l\":\"YYYY-MM-DD\"},"
                     "{\"v\":4,\"l\":\"DD.MM.YYYY\"}]},"
      "{\"id\":\"timeFormat\",\"type\":\"enum\",\"label\":\"Time format\","
        "\"options\":[{\"v\":0,\"l\":\"HH:MM:SS\"},{\"v\":1,\"l\":\"HH:MM\"},{\"v\":2,\"l\":\"12h\"}]},"
      "{\"id\":\"endFormat\",\"type\":\"enum\",\"label\":\"End column\","
        "\"options\":[{\"v\":0,\"l\":\"End time\"},{\"v\":1,\"l\":\"Duration\"},{\"v\":2,\"l\":\"Off\"}]},"
      "{\"id\":\"volumeFormat\",\"type\":\"enum\",\"label\":\"Volume format\","
        "\"options\":[{\"v\":0,\"l\":\"L (comma)\"},{\"v\":1,\"l\":\"L (dot)\"},"
                     "{\"v\":2,\"l\":\"Number only\"},{\"v\":3,\"l\":\"Off\"}]},"
      "{\"id\":\"manualPressThresholdMs\",\"type\":\"int\",\"min\":0,\"max\":60000,"
        "\"label\":\"Manual-press hold (ms)\"},"
      "{\"id\":\"postCorrectionEnabled\",\"type\":\"bool\",\"label\":\"Post-correction\"},"
      "{\"id\":\"pfToFfThreshold\",\"type\":\"float\",\"min\":0,\"max\":1000,"
        "\"label\":\"PF→FF threshold (L)\",\"showIf\":\"postCorrectionEnabled\"},"
      "{\"id\":\"ffToPfThreshold\",\"type\":\"float\",\"min\":0,\"max\":1000,"
        "\"label\":\"FF→PF threshold (L)\",\"showIf\":\"postCorrectionEnabled\"}"
    "]}";

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
bool DataLogModule::load(JsonObjectConst cfg) {
    DatalogConfig& d = config.datalog;
    copyStr(d.prefix,       sizeof(d.prefix),       cfg["prefix"]      | (const char*)nullptr);
    copyStr(d.folder,       sizeof(d.folder),       cfg["folder"]      | (const char*)nullptr);
    copyStr(d.currentFile,  sizeof(d.currentFile),  cfg["currentFile"] | (const char*)nullptr);

    d.rotation              = (DatalogRotation)(cfg["rotation"] | (int)d.rotation);
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
    d.dateFormat            = (uint8_t)(cfg["dateFormat"]   | (int)d.dateFormat);
    if (d.dateFormat > 4) d.dateFormat = 0;
    d.timeFormat            = (uint8_t)(cfg["timeFormat"]   | (int)d.timeFormat);
    if (d.timeFormat > 2) d.timeFormat = 0;
    d.endFormat             = (uint8_t)(cfg["endFormat"]    | (int)d.endFormat);
    if (d.endFormat > 2) d.endFormat = 0;
    d.volumeFormat          = (uint8_t)(cfg["volumeFormat"] | (int)d.volumeFormat);
    if (d.volumeFormat > 3) d.volumeFormat = 0;
    if (cfg["manualPressThresholdMs"].is<int>()) {
        int v = cfg["manualPressThresholdMs"].as<int>();
        d.manualPressThresholdMs = (v < 0) ? 0 : (v > 60000 ? 60000 : (uint16_t)v);
    }
    d.postCorrectionEnabled = cfg["postCorrectionEnabled"]  | d.postCorrectionEnabled;
    d.pfToFfThreshold       = cfg["pfToFfThreshold"] | d.pfToFfThreshold;
    if (!(d.pfToFfThreshold >= 0.1f && d.pfToFfThreshold <= 1000.0f)) d.pfToFfThreshold = 4.5f;
    d.ffToPfThreshold       = cfg["ffToPfThreshold"] | d.ffToPfThreshold;
    if (!(d.ffToPfThreshold >= 0.1f && d.ffToPfThreshold <= 1000.0f)) d.ffToPfThreshold = 3.7f;
    return true;
}

// ---------------------------------------------------------------------------
bool DataLogModule::save(JsonObject cfg) const {
    const DatalogConfig& d = config.datalog;
    cfg["prefix"]                 = d.prefix;
    cfg["folder"]                 = d.folder;
    cfg["rotation"]               = (int)d.rotation;
    cfg["maxSizeKB"]              = d.maxSizeKB;
    cfg["maxEntries"]             = d.maxEntries;
    cfg["timestampFilename"]      = d.timestampFilename;
    cfg["includeDeviceId"]        = d.includeDeviceId;
    cfg["includeBootCount"]       = d.includeBootCount;
    cfg["includeExtraPresses"]    = d.includeExtraPresses;
    cfg["dateFormat"]             = (int)d.dateFormat;
    cfg["timeFormat"]             = (int)d.timeFormat;
    cfg["endFormat"]              = (int)d.endFormat;
    cfg["volumeFormat"]           = (int)d.volumeFormat;
    cfg["manualPressThresholdMs"] = d.manualPressThresholdMs;
    cfg["postCorrectionEnabled"]  = d.postCorrectionEnabled;
    cfg["pfToFfThreshold"]        = d.pfToFfThreshold;
    cfg["ffToPfThreshold"]        = d.ffToPfThreshold;
    return true;
}

// ---------------------------------------------------------------------------
const char* DataLogModule::schema() const {
    return DATALOG_SCHEMA;
}
