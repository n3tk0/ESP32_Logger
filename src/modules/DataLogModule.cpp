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
      "{\"id\":\"maxEntries\",\"type\":\"int\",\"min\":0,\"max\":65535,\"label\":\"Max entries\"},"
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
bool DataLogModule::load(JsonObjectConst cfg) {
    DatalogConfig& d = config.datalog;
    copyStr(d.prefix, sizeof(d.prefix), cfg["prefix"] | (const char*)nullptr);
    copyStr(d.folder, sizeof(d.folder), cfg["folder"] | (const char*)nullptr);

    d.rotation              = (DatalogRotation)(cfg["rotation"] | (int)d.rotation);
    d.maxSizeKB             = cfg["maxSizeKB"]  | d.maxSizeKB;
    d.maxEntries            = cfg["maxEntries"] | d.maxEntries;
    d.timestampFilename     = cfg["timestampFilename"]   | d.timestampFilename;
    d.includeDeviceId       = cfg["includeDeviceId"]     | d.includeDeviceId;
    d.includeBootCount      = cfg["includeBootCount"]    | d.includeBootCount;
    d.includeExtraPresses   = cfg["includeExtraPresses"] | d.includeExtraPresses;
    d.dateFormat            = (uint8_t)(cfg["dateFormat"]   | (int)d.dateFormat);
    d.timeFormat            = (uint8_t)(cfg["timeFormat"]   | (int)d.timeFormat);
    d.endFormat             = (uint8_t)(cfg["endFormat"]    | (int)d.endFormat);
    d.volumeFormat          = (uint8_t)(cfg["volumeFormat"] | (int)d.volumeFormat);
    d.manualPressThresholdMs= cfg["manualPressThresholdMs"] | d.manualPressThresholdMs;
    d.postCorrectionEnabled = cfg["postCorrectionEnabled"]  | d.postCorrectionEnabled;
    d.pfToFfThreshold       = cfg["pfToFfThreshold"] | d.pfToFfThreshold;
    d.ffToPfThreshold       = cfg["ffToPfThreshold"] | d.ffToPfThreshold;
    return true;
}

// ---------------------------------------------------------------------------
void DataLogModule::save(JsonObject cfg) const {
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
}

// ---------------------------------------------------------------------------
const char* DataLogModule::schema() const {
    return DATALOG_SCHEMA;
}
