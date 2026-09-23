#include "NodeStore.h"
#include "NodeLog.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "node_config.h"
#include "src/nodecfg/NodeConfigJson.h"

using nodecfg::NodeConfig;
using nodecfg::SensorCfg;
using nodecfg::SensorType;
using nodecfg::copyStr;

static const char* CFG_PATH  = "/config.json";
static const char* PREV_PATH = "/config.prev.json";
static const char* TMP_PATH  = "/config.tmp";
static const char* PTMP_PATH = "/config.prev.tmp";
static const char* SYNC_PATH = "/sync.json";
static const char* STMP_PATH = "/sync.tmp";

// ---------------------------------------------------------------------------
// Defaults: node_config.h, as build flags have always set it
// ---------------------------------------------------------------------------

/// The facts this firmware reports about itself. Never taken from a file: a
/// config copied from another node must not make this one claim to be it.
static void stampIdentity(NodeConfig& c) {
    c.transport = nodecfg::Transport::Wifi;
    c.hw        = nodecfg::Hw::Esp8266;
    copyStr(c.fw, sizeof(c.fw), NODE_FW_VERSION);
}

static void seedDefaults(NodeConfig& c) {
    c = nodecfg::configDefaults(nodecfg::Transport::Wifi, nodecfg::Hw::Esp8266);
    stampIdentity(c);

    copyStr(c.net.ssid,       sizeof(c.net.ssid),       WIFI_SSID);
    copyStr(c.net.pass,       sizeof(c.net.pass),       WIFI_PASS);
    copyStr(c.net.host,       sizeof(c.net.host),       COLLECTOR_HOST);
    copyStr(c.net.token,      sizeof(c.net.token),      INGEST_TOKEN);
    copyStr(c.net.basic_user, sizeof(c.net.basic_user), COLLECTOR_BASIC_USER);
    copyStr(c.net.basic_pass, sizeof(c.net.basic_pass), COLLECTOR_BASIC_PASS);
    copyStr(c.name,           sizeof(c.name),           NODE_ID);
    c.net.port = COLLECTOR_PORT;

    // The placeholder SSID is not a network. Treating it as one would make the
    // config look complete and send the node into a retry loop against an AP
    // that does not exist, instead of opening the portal.
    if (strcmp(c.net.ssid, "your-ssid") == 0) {
        c.net.ssid[0] = '\0';
        c.net.pass[0] = '\0';
    }

    unsigned long secs = ((unsigned long)POST_INTERVAL_MS + 500UL) / 1000UL;
    if (secs < nodecfg::INTERVAL_MIN_S) secs = nodecfg::INTERVAL_MIN_S;
    if (secs > nodecfg::INTERVAL_MAX_S) secs = nodecfg::INTERVAL_MAX_S;
    c.interval_s = (uint16_t)secs;
    c.altitude_m = ALTITUDE_M;
    c.i2c.sda    = I2C_SDA_PIN;
    c.i2c.scl    = I2C_SCL_PIN;

    // The sensor list the build selected, in the order node/ has always read
    // them. With nothing selected the list is empty and the node reports no
    // readings until one is added — on its page or from the collector.
    c.sensor_count = 0;
#ifdef NODE_SENSOR_BMX280
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Bmx280, c.hw);
        // 0x76 was always "probe 0x76, then 0x77" — which is what 0 means now.
        s.addr = (BMX280_ADDR == 0x76) ? 0 : BMX280_ADDR;
        nodecfg::addSensor(c, s);
    }
#endif
#ifdef NODE_SENSOR_BME688
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Bme688, c.hw);
        // BMX280_ADDR on purpose: it is the one I2C address macro for both
        // (node_config.h says why).
        s.addr = (BMX280_ADDR == 0x76) ? 0 : BMX280_ADDR;
        nodecfg::addSensor(c, s);
    }
#endif
#ifdef NODE_SENSOR_BH1750
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Bh1750, c.hw);
        s.addr = BH1750_ADDR;
        nodecfg::addSensor(c, s);
    }
#endif
#ifdef NODE_SENSOR_SDS011
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Sds011, c.hw);
        s.rx = SDS011_RX_PIN;
        s.tx = SDS011_TX_PIN;
        nodecfg::addSensor(c, s);
    }
#endif
#ifdef NODE_SENSOR_PULSE
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Pulse, c.hw);
        s.pin         = PULSE_PIN;
        s.mode        = PULSE_MODE_RAIN ? nodecfg::PulseMode::Rain : nodecfg::PulseMode::Flow;
        s.per_pulse   = PULSE_UNITS_PER_PULSE;
        s.debounce_us = PULSE_DEBOUNCE_US;
        nodecfg::addSensor(c, s);
    }
#endif
#ifdef NODE_SENSOR_DS18B20
    {
        SensorCfg s = nodecfg::sensorDefaults(SensorType::Ds18b20, c.hw);
        s.pin   = ONEWIRE_PIN;
        s.count = NODE_DS18B20_EXPECTED;
        copyStr(s.metric, sizeof(s.metric), NODE_DS18B20_METRIC);
        nodecfg::addSensor(c, s);
    }
#endif
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static bool mountFs() {
    static bool mounted = false;
    if (mounted) return true;
    // A node fresh from the factory has no filesystem image; formatting on
    // first mount is the difference between "portal comes up" and "nothing
    // works and the serial log says FS error".
    if (!LittleFS.begin()) {
        LOGLN("[cfg] LittleFS mount failed, formatting");
        if (!LittleFS.format() || !LittleFS.begin()) {
            LOGLN("[cfg] format failed; running on defaults only");
            return false;
        }
    }
    mounted = true;
    return true;
}

/// Temp file + rename. No remove() of the destination first: LittleFS's
/// rename replaces it atomically, and deleting the good file and then
/// renaming opened exactly the window this dance exists to close — a
/// brownout in between left the node with no config at all.
static bool commit(const char* tmp, const char* dst) {
    if (!LittleFS.rename(tmp, dst)) {
        LOGF("[cfg] rename %s -> %s failed\n", tmp, dst);
        LittleFS.remove(tmp);
        return false;
    }
    return true;
}

/// Copy `src` to `dst` through `tmp`.
static bool copyFile(const char* src, const char* dst, const char* tmp) {
    File in = LittleFS.open(src, "r");
    if (!in) return false;
    LittleFS.remove(tmp);
    File out = LittleFS.open(tmp, "w");
    if (!out) { in.close(); return false; }
    uint8_t buf[128];
    size_t total = 0;
    bool ok = true;
    while (in.available()) {
        const size_t n = in.read(buf, sizeof(buf));
        if (n == 0) break;
        if (out.write(buf, n) != n) { ok = false; break; }
        total += n;
    }
    in.close();
    out.close();
    if (!ok || total == 0) {
        LittleFS.remove(tmp);
        return false;
    }
    return commit(tmp, dst);
}

enum class ReadResult { Missing, Bad, Ok, Migrated };

static ReadResult readConfigFile(const char* path, NodeConfig& cfg) {
    File f = LittleFS.open(path, "r");
    if (!f) return ReadResult::Missing;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        LOGF("[cfg] %s is corrupt (%s)\n", path, err.c_str());
        return ReadResult::Bad;
    }
    if (nodecfg::isLegacyWifiDoc(doc)) {
        // The pre-§1 flat file. The sensor list it never had comes from the
        // NODE_SENSOR_* seed already in `cfg`; the pins it did have are moved
        // onto those entries.
        nodecfg::migrateLegacyWifi(doc, cfg);
        return ReadResult::Migrated;
    }
    nodecfg::Issue issue;
    if (!nodecfg::decodeConfig(doc, cfg, nodecfg::NCJ_DEC_REV, &issue)) {
        LOGF("[cfg] %s refused: %s: %s\n", path, issue.field, issue.reason);
        return ReadResult::Bad;
    }
    return ReadResult::Ok;
}

bool storeLoad(NodeConfig& out) {
    seedDefaults(out);
    if (!mountFs()) return false;

    NodeConfig tmp = out;
    ReadResult r = readConfigFile(CFG_PATH, tmp);
    if (r == ReadResult::Bad) {
        // A config that cannot be read is not a reason to forget the node's
        // network: the copy from before the last save is the next best thing.
        tmp = out;
        const ReadResult p = readConfigFile(PREV_PATH, tmp);
        if (p == ReadResult::Ok || p == ReadResult::Migrated) {
            LOGLN("[cfg] using /config.prev.json");
            r = p;
        }
    }
    if (r != ReadResult::Ok && r != ReadResult::Migrated) return false;

    out = tmp;
    stampIdentity(out);
    if (r == ReadResult::Migrated) {
        LOGLN("[cfg] migrated the old /config.json to the new format");
        storeSave(out, true);   // the old file becomes /config.prev.json
    }
    return true;
}

bool storeSave(const NodeConfig& c, bool backup) {
    if (!mountFs()) return false;

    size_t written = 0;
    {
        JsonDocument doc;
        nodecfg::encodeConfig(c, doc.to<JsonObject>(), nodecfg::NCJ_SECRETS);
        if (doc.overflowed()) {
            LOGLN("[cfg] out of memory encoding the config");
            return false;
        }
        LittleFS.remove(TMP_PATH);
        File f = LittleFS.open(TMP_PATH, "w");
        if (!f) {
            LOGLN("[cfg] cannot open temp file");
            return false;
        }
        written = serializeJson(doc, f);
        f.close();
    }
    if (written == 0) {
        LOGLN("[cfg] write produced no bytes");
        LittleFS.remove(TMP_PATH);
        return false;
    }

    // The backup is taken only once the new file is safely written, and
    // failing to take it does not stop the save: a node that cannot back up
    // should still be able to change its settings.
    if (backup && LittleFS.exists(CFG_PATH)) {
        if (!copyFile(CFG_PATH, PREV_PATH, PTMP_PATH))
            LOGLN("[cfg] could not write /config.prev.json");
    }

    if (!commit(TMP_PATH, CFG_PATH)) return false;
    LOGF("[cfg] saved %u bytes (rev %u%s)\n", (unsigned)written,
                  (unsigned)c.rev, c.local ? ", local" : "");
    return true;
}

bool storeRollback() {
    if (!mountFs() || !LittleFS.exists(PREV_PATH)) return false;
    return copyFile(PREV_PATH, CFG_PATH, TMP_PATH);
}

bool storeIsComplete(const NodeConfig& c) {
    return c.net.ssid[0] != '\0' && c.net.host[0] != '\0';
}

// ---------------------------------------------------------------------------
// /sync.json
// ---------------------------------------------------------------------------

bool syncLoad(SyncState& out) {
    out = SyncState();
    if (!mountFs()) return false;
    File f = LittleFS.open(SYNC_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    out.trialRev = doc["trial_rev"] | 0;
    JsonObjectConst e = doc["err"];
    if (!e.isNull()) {
        const uint16_t rev = e["rev"] | 0;
        if (rev) out.err.set(rev, e["field"] | "", e["reason"] | "");
    }
    return true;
}

bool syncSave(const SyncState& s) {
    if (!mountFs()) return false;
    if (s.trialRev == 0 && !s.err.pending()) {
        LittleFS.remove(SYNC_PATH);
        return true;
    }
    JsonDocument doc;
    doc["trial_rev"] = s.trialRev;
    if (s.err.pending()) {
        JsonObject e = doc["err"].to<JsonObject>();
        e["rev"]    = s.err.rev;
        e["field"]  = (const char*)s.err.field;
        e["reason"] = (const char*)s.err.reason;
    }
    LittleFS.remove(STMP_PATH);
    File f = LittleFS.open(STMP_PATH, "w");
    if (!f) return false;
    const size_t n = serializeJson(doc, f);
    f.close();
    if (n == 0) { LittleFS.remove(STMP_PATH); return false; }
    return commit(STMP_PATH, SYNC_PATH);
}
