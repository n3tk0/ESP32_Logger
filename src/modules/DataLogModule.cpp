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
      "{\"id\":\"prefix\",\"type\":\"string\",\"max\":32,\"label\":\"Filename prefix\",\"group\":\"File & rotation\"},"
      "{\"id\":\"folder\",\"type\":\"string\",\"max\":32,\"label\":\"Folder\",\"group\":\"File & rotation\"},"
      "{\"id\":\"rotation\",\"type\":\"enum\",\"label\":\"Rotation\",\"group\":\"File & rotation\","
        "\"options\":[{\"v\":0,\"l\":\"None\"},{\"v\":1,\"l\":\"Daily\"},"
                     "{\"v\":2,\"l\":\"Weekly\"},{\"v\":3,\"l\":\"Monthly\"},"
                     "{\"v\":4,\"l\":\"By size\"}]},"
      "{\"id\":\"maxSizeKB\",\"type\":\"int\",\"min\":0,\"max\":1048576,\"label\":\"Max size\",\"unit\":\"KB\",\"group\":\"File & rotation\","
        "\"showIf\":{\"rotation\":4}},"
      "{\"id\":\"maxEntries\",\"type\":\"int\",\"min\":10,\"max\":65535,\"label\":\"Max entries\",\"unit\":\"rows\",\"group\":\"File & rotation\","
        "\"help\":\"Oldest rows are trimmed once the file exceeds this many entries.\"},"
      "{\"id\":\"timestampFilename\",\"type\":\"bool\",\"label\":\"Timestamp in filename\",\"group\":\"File & rotation\"},"
      "{\"id\":\"includeDeviceId\",\"type\":\"bool\",\"label\":\"Include device ID\",\"group\":\"File & rotation\"},"
      "{\"id\":\"includeBootCount\",\"type\":\"bool\",\"label\":\"Include boot count\",\"group\":\"File & rotation\"},"
      "{\"id\":\"includeExtraPresses\",\"type\":\"bool\",\"label\":\"Log extra presses\",\"group\":\"File & rotation\"},"
      "{\"id\":\"dateFormat\",\"type\":\"enum\",\"label\":\"Date format\",\"group\":\"Column format\","
        "\"options\":[{\"v\":0,\"l\":\"Off\"},{\"v\":1,\"l\":\"DD/MM/YYYY\"},"
                     "{\"v\":2,\"l\":\"MM/DD/YYYY\"},{\"v\":3,\"l\":\"YYYY-MM-DD\"},"
                     "{\"v\":4,\"l\":\"DD.MM.YYYY\"}]},"
      "{\"id\":\"timeFormat\",\"type\":\"enum\",\"label\":\"Time format\",\"group\":\"Column format\","
        "\"options\":[{\"v\":0,\"l\":\"HH:MM:SS\"},{\"v\":1,\"l\":\"HH:MM\"},{\"v\":2,\"l\":\"12h\"}]},"
      "{\"id\":\"endFormat\",\"type\":\"enum\",\"label\":\"End column\",\"group\":\"Column format\","
        "\"options\":[{\"v\":0,\"l\":\"End time\"},{\"v\":1,\"l\":\"Duration\"},{\"v\":2,\"l\":\"Off\"}]},"
      "{\"id\":\"volumeFormat\",\"type\":\"enum\",\"label\":\"Volume format\",\"group\":\"Column format\","
        "\"options\":[{\"v\":0,\"l\":\"L (comma)\"},{\"v\":1,\"l\":\"L (dot)\"},"
                     "{\"v\":2,\"l\":\"Number only\"},{\"v\":3,\"l\":\"Off\"}]},"
      "{\"id\":\"manualPressThresholdMs\",\"type\":\"int\",\"min\":0,\"max\":60000,\"unit\":\"ms\",\"group\":\"Water logging\","
        "\"label\":\"Manual-press hold\"},"
      "{\"id\":\"postCorrectionEnabled\",\"type\":\"bool\",\"label\":\"Post-correction\",\"group\":\"Water logging\"},"
      "{\"id\":\"pfToFfThreshold\",\"type\":\"float\",\"min\":0,\"max\":1000,\"unit\":\"L\",\"group\":\"Water logging\","
        "\"label\":\"PF→FF threshold\",\"showIf\":\"postCorrectionEnabled\"},"
      "{\"id\":\"ffToPfThreshold\",\"type\":\"float\",\"min\":0,\"max\":1000,\"unit\":\"L\",\"group\":\"Water logging\","
        "\"label\":\"FF→PF threshold\",\"showIf\":\"postCorrectionEnabled\"}"
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
