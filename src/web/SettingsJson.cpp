// src/web/SettingsJson.cpp — see SettingsJson.h.
#include "SettingsJson.h"
#include "KindleConfigJson.h"
#include "../core/Config.h"
#include "../modules/ThemeModule.h"
#include "../modules/DataLogModule.h"
#include "../modules/WiFiModule.h"
#include "../modules/TimeModule.h"
#include "../storage/Datalog.h"          // the data log's sensor columns
#include "../utils/Utils.h"              // sanitizePath
#include "../utils/JsonEnum.h"           // jsonEnumInt
#include "../utils/WifiTxPower.h"        // wifiTxPowerValid
#include "../utils/CpuFreq.h"            // cpuMhzValid, CPU_MAX_MHZ

#include <string.h>

namespace {

void ipToJson(JsonObject o, const char* key, const uint8_t ip[4]) {
    char buf[16];
    formatIPv4(ip, buf, sizeof(buf));
    o[key] = buf;
}

void copyIfString(JsonObjectConst o, const char* key, char* dst, size_t n) {
    const char* v = o[key] | (const char*)nullptr;
    if (!v) return;
    strncpy(dst, v, n - 1);
    dst[n - 1] = '\0';
}

// The data log section through DataLogModule::load(), which trusts that its
// paths were checked: the prefix and folder get the same checks as the form,
// and a value that fails keeps what was stored. currentFile is not part of
// the file (it names a file on this device's storage) and is never taken
// from one.
void datalogFromJson(JsonObjectConst dl) {
    DatalogConfig& d = config.datalog;
    DatalogConfig before = d;
    // load() cuts a string to fit, so the lengths are checked on the file's
    // own text: a prefix or folder too long for the form is refused, not
    // shortened into a name nobody chose.
    const char* prefix = dl["prefix"] | (const char*)nullptr;
    const char* folder = dl["folder"] | (const char*)nullptr;
    DataLogModule::instance().load(dl);

    memcpy(d.currentFile, before.currentFile, sizeof(d.currentFile));
    if ((prefix && !datalogPrefixOk(prefix, strlen(prefix))) ||
        !datalogPrefixOk(d.prefix, strlen(d.prefix)))
        memcpy(d.prefix, before.prefix, sizeof(d.prefix));
    if (folder && strlen(folder) >= sizeof(d.folder))
        memcpy(d.folder, before.folder, sizeof(d.folder));
    else if (d.folder[0]) {
        const String clean = sanitizePath(String(d.folder));
        if (clean.length() == 0 || clean.length() >= sizeof(d.folder))
            memcpy(d.folder, before.folder, sizeof(d.folder));
        else
            memcpy(d.folder, clean.c_str(), clean.length() + 1);
    }
}

} // namespace

void settingsToJson(JsonObject doc, bool revealSecrets) {
    // ── Identity ──────────────────────────────────────────────────────────
    doc["deviceName"]     = strlen(config.deviceName) ? config.deviceName : "Water Logger";
    doc["deviceId"]       = config.deviceId;
    doc["forceWebServer"] = config.forceWebServer;

    // ── Theme ─────────────────────────────────────────────────────────────
    // The module's fields, then the colours and the board picture it does
    // not carry.
    JsonObject th = doc["theme"].to<JsonObject>();
    ThemeModule::instance().save(th);
    th["ffColor"]           = config.theme.ffColor;
    th["pfColor"]           = config.theme.pfColor;
    th["otherColor"]        = config.theme.otherColor;
    th["storageBarColor"]   = config.theme.storageBarColor;
    th["storageBar70Color"] = config.theme.storageBar70Color;
    th["storageBar90Color"] = config.theme.storageBar90Color;
    th["storageBarBorder"]  = config.theme.storageBarBorder;
    th["boardDiagramPath"]  = config.theme.boardDiagramPath;

    // ── Flow Meter ────────────────────────────────────────────────────────
    JsonObject fm = doc["flowMeter"].to<JsonObject>();
    fm["pulsesPerLiter"]        = config.flowMeter.pulsesPerLiter > 0    ? config.flowMeter.pulsesPerLiter    : 450.0f;
    fm["calibrationMultiplier"] = config.flowMeter.calibrationMultiplier ? config.flowMeter.calibrationMultiplier : 1.0f;
    fm["testMode"]              = config.flowMeter.testMode;
    fm["blinkDuration"]         = config.flowMeter.blinkDuration > 0 ? config.flowMeter.blinkDuration : 250;

    // ── Datalog ───────────────────────────────────────────────────────────
    JsonObject dl = doc["datalog"].to<JsonObject>();
    DataLogModule::instance().save(dl);
    datalogColsToJson(dl["sensorCols"].to<JsonObject>());
    {
        // What this device can put in the log, for the page to offer only
        // that. Read-only: an import ignores it.
        const DatalogLayout all = datalogLayout(true);
        JsonObject av = dl["avail"].to<JsonObject>();
        av["volume"] = all.volume;
        av["ff"]     = all.ff;
        av["pf"]     = all.pf;
    }

    // ── Logger (the data log's sensor rows) ────────────────────────────────
    JsonObject lg = doc["logger"].to<JsonObject>();
    lg["csvLoggingEnabled"]      = config.logger.csvLoggingEnabled;
    lg["aggregationIntervalSec"] = config.logger.aggregationIntervalSec ? config.logger.aggregationIntervalSec : 60;

    // ── Kindle dashboard appearance ───────────────────────────────────────
    // Exported on every build, including one without FEATURE_KINDLE_DASHBOARD:
    // a settings file is a record of the device's configuration, and dropping
    // a section because this particular firmware cannot draw it would mean a
    // backup taken on one build quietly resetting the appearance on another.
    kdConfigToFile(config.kindle, doc["kindle"].to<JsonObject>());

    // ── Network ───────────────────────────────────────────────────────────
    // The client side and the clock from their modules; the access point,
    // which no module carries, here. Passwords only on request.
    JsonObject net = doc["network"].to<JsonObject>();
    WiFiModule::instance().save(net);
    TimeModule::instance().save(net);
    net["apSSID"]         = strlen(config.network.apSSID) ? config.network.apSSID : DEFAULT_AP_SSID;
    net["apPassword"]     = revealSecrets ? config.network.apPassword     : "***";
    net["clientPassword"] = revealSecrets ? config.network.clientPassword : "***";
    ipToJson(net, "apIP",      config.network.apIP);
    ipToJson(net, "apGateway", config.network.apGateway);
    ipToJson(net, "apSubnet",  config.network.apSubnet);

    // ── Hardware ──────────────────────────────────────────────────────────
    JsonObject hw = doc["hardware"].to<JsonObject>();
    hw["storageType"]        = (int)config.hardware.storageType;
    hw["wakeupMode"]         = (int)config.hardware.wakeupMode;
    hw["cpuFreqMHz"]         = config.hardware.cpuFreqMHz > 0 ? config.hardware.cpuFreqMHz : 80;
    hw["activeCpuMHz"]       = config.hardware.activeCpuMHz;   // 0 = default (160)
    hw["cpuMaxMHz"]          = CPU_MAX_MHZ;     // what the form may offer; not imported
    hw["defaultStorageView"] = config.hardware.defaultStorageView;
    hw["debounceMs"]         = config.hardware.debounceMs > 0  ? config.hardware.debounceMs : 100;
    hw["pinWifiTrigger"]     = config.hardware.pinWifiTrigger;
    hw["pinWakeupFF"]        = config.hardware.pinWakeupFF;
    hw["pinWakeupPF"]        = config.hardware.pinWakeupPF;
    hw["pinFlowSensor"]      = config.hardware.pinFlowSensor;
    hw["pinRtcCE"]           = config.hardware.pinRtcCE;
    hw["pinRtcIO"]           = config.hardware.pinRtcIO;
    hw["pinRtcSCLK"]         = config.hardware.pinRtcSCLK;
    hw["pinSdCS"]            = config.hardware.pinSdCS;
    hw["pinSdMOSI"]          = config.hardware.pinSdMOSI;
    hw["pinSdMISO"]          = config.hardware.pinSdMISO;
    hw["pinSdSCK"]           = config.hardware.pinSdSCK;
}

void settingsFromJson(JsonObjectConst doc) {
    copyIfString(doc, "deviceName", config.deviceName, sizeof(config.deviceName));
    if (doc["forceWebServer"].is<bool>()) config.forceWebServer = doc["forceWebServer"];

    JsonObjectConst t = doc["theme"];
    if (t) {
        // The module checks the colours are colours and the paths are paths.
        ThemeModule::instance().load(t);
        copyIfString(t, "ffColor",    config.theme.ffColor,    sizeof(config.theme.ffColor));
        copyIfString(t, "pfColor",    config.theme.pfColor,    sizeof(config.theme.pfColor));
        copyIfString(t, "otherColor", config.theme.otherColor, sizeof(config.theme.otherColor));
    }

    JsonObjectConst fm = doc["flowMeter"];
    if (fm) {
        if (fm["pulsesPerLiter"].is<float>())        config.flowMeter.pulsesPerLiter        = fm["pulsesPerLiter"];
        if (fm["calibrationMultiplier"].is<float>()) config.flowMeter.calibrationMultiplier = fm["calibrationMultiplier"];
    }

    JsonObjectConst dl = doc["datalog"];
    if (dl) {
        datalogFromJson(dl);
        if (dl["sensorCols"].is<JsonObjectConst>()) datalogColsFromJson(dl["sensorCols"]);
    }

    // The clock through its module; the WiFi mode and static-IP switch as
    // before. The client SSID, the addresses and the passwords are NOT taken
    // from a file: the passwords arrive as "***", and restoring the rest
    // without them would point the device at a network it cannot join.
    JsonObjectConst net = doc["network"];
    if (net) {
        TimeModule::instance().load(net);   // also re-applies the clock
        const int mode = net["wifiMode"] | -1;
        if (mode == WIFIMODE_AP || mode == WIFIMODE_CLIENT) config.network.wifiMode = (WiFiModeType)mode;
        if (net["useStaticIP"].is<bool>()) config.network.useStaticIP = net["useStaticIP"];
        // A power this firmware does not offer keeps what was stored.
        const int tx = jsonEnumInt(net["txPower"], -1);
        if (wifiTxPowerValid(tx)) config.network.txPower = (uint8_t)tx;
    }

    JsonObjectConst hw = doc["hardware"];
    if (hw) {
        // An enum outside its values keeps what was stored; debounce gets
        // the form's 20..500 ms.
        const int st = hw["storageType"] | -1;
        const int wk = hw["wakeupMode"]  | -1;
        if (st == STORAGE_LITTLEFS || st == STORAGE_SD_CARD)                config.hardware.storageType = (StorageType)st;
        if (wk == WAKEUP_GPIO_ACTIVE_HIGH || wk == WAKEUP_GPIO_ACTIVE_LOW)  config.hardware.wakeupMode  = (WakeupMode)wk;
        if (hw["cpuFreqMHz"].is<int>() && cpuMhzValid(hw["cpuFreqMHz"].as<int>()))
            config.hardware.cpuFreqMHz = hw["cpuFreqMHz"];
        // 240 from an S3 backup imported on a C3 keeps what was stored.
        if (hw["activeCpuMHz"].is<int>()) {
            const int v = hw["activeCpuMHz"].as<int>();
            if (v == 0 || cpuMhzValid(v)) config.hardware.activeCpuMHz = (uint8_t)v;
        }
        if (hw["defaultStorageView"].is<int>()) config.hardware.defaultStorageView = hw["defaultStorageView"];
        if (hw["debounceMs"].is<int>()) {
            const int v = hw["debounceMs"].as<int>();
            config.hardware.debounceMs = v < 20 ? 20 : (v > 500 ? 500 : v);
        }
        if (hw["debugMode"].is<bool>())         config.hardware.debugMode          = hw["debugMode"].as<bool>();
    }

    JsonObjectConst lg = doc["logger"];
    if (lg) {
        if (lg["csvLoggingEnabled"].is<bool>()) config.logger.csvLoggingEnabled = lg["csvLoggingEnabled"];
        if (lg["aggregationIntervalSec"].is<int>()) {
            const int v = lg["aggregationIntervalSec"].as<int>();
            config.logger.aggregationIntervalSec = v < 5 ? 5 : (v > 3600 ? 3600 : v);
        }
    }

    JsonObjectConst kd = doc["kindle"];
    if (kd) kdConfigFromFile(config.kindle, kd);
}
