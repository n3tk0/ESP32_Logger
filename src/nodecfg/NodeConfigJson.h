// ============================================================================
// src/nodecfg/NodeConfigJson.h
//
// The §1 document <-> NodeConfig, both ways, plus the old flat /config.json
// and the page's `caps`.
//
// EVERY JSON EDGE GOES THROUGH HERE
// ---------------------------------
// The WiFi node's /config.json, its ingest request and reply, both nodes'
// /api/config, the collector's /nodes/*.json and /api/nodes/config, and the
// ESP-NOW CFG / CFG_REPORT slices are all this one document. One encoder and
// one decoder means a key spelled differently in one place is impossible,
// and the rules that are easy to get subtly wrong — secrets are write-only,
// "" means keep, a missing key means keep — are written once.
//
// THE SECRET RULES (docs/NODE_CONFIG.md §0.5)
// -------------------------------------------
// net.pass, net.token, net.basic_pass, net.next.pass (and, on the ESP-NOW
// node's own page, lmk) are write-only. Encoding without NCJ_SECRETS writes
// each as "" with a sibling "<key>_set": true|false. Decoding:
//
//   "pass": "hunter22"                   set it
//   "pass": ""   (or "pass" missing)     keep the stored one
//   "pass": "",  "pass_set": false       CLEAR it
//
// The third line is an addition to §0.5 — without it an optional secret
// (basic_pass, or a passphrase for a network that has become open) could be
// set once and never removed. It is exactly what echoing a GET back produces
// for a secret that was never set, so a page that sends back what it was
// given changes nothing. Encoding WITH secrets always writes the *_set flags
// too, so the collector sending "basic_pass": "" to a WiFi node means "none",
// not "keep yours".
//
// PARTIAL DOCUMENTS
// -----------------
// decodeConfig() changes only what the document mentions. `sensors`, when
// present, replaces the whole list — merging arrays by position is a guess —
// but an entry that keeps its position and type keeps its unmentioned fields.
// Decoding is all-or-nothing: on an error `cfg` is untouched.
//
// Needs ArduinoJson v7 — the firmwares get it from lib_deps, tests/host from
// tests/host/vendor/ via the shim include path.
// ============================================================================
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

#include "NodeConfig.h"
#include "HwPins.h"
#include "MetricCatalog.h"
#include "NodeConfigValidate.h"

namespace nodecfg {

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

enum EncodeFlags : uint8_t {
    /// Write the secrets themselves. ONLY for: the WiFi node's own
    /// /config.json, the collector's stored `desired`, and the collector's
    /// ingest reply to a WiFi node. Every GET and every report is without.
    NCJ_SECRETS = 1 << 0,
    /// Write "lmk": "" and "lmk_set" (never the key). Only the ESP-NOW node's
    /// own GET /api/config; never on the radio, never at the collector.
    NCJ_LMK     = 1 << 1,
};

enum DecodeFlags : uint8_t {
    /// Read `rev` and `local`. For: a node applying a config from the
    /// collector (the rev it came from), the collector reading a node's report.
    /// NOT for a page POST, where a stale rev must not overwrite the real one.
    NCJ_DEC_REV      = 1 << 0,
    /// Read `transport`, `hw` and `fw`. For the collector reading what a node
    /// reports about itself. A node never takes these from outside.
    NCJ_DEC_IDENTITY = 1 << 1,
    /// Accept `lmk`. Only the ESP-NOW node's own POST /api/config.
    NCJ_DEC_LMK      = 1 << 2,
};

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

namespace detail {

static inline void putSecret(JsonObject o, const char* key, const char* setKey,
                             const char* value, bool withSecrets) {
    const bool isSet = value && value[0];
    o[key] = withSecrets ? value : "";
    o[setKey] = isSet;
}

static inline void encodeSensor(const SensorCfg& s, JsonObject o) {
    o["type"] = sensorTypeName(s.type);
    switch (s.type) {
        case SensorType::Bmx280:
        case SensorType::Bme688:
        case SensorType::Bh1750:
            o["addr"] = s.addr;
            break;
        case SensorType::Ds18b20:
            o["pin"]    = s.pin;
            o["count"]  = s.count;
            o["metric"] = (const char*)s.metric;   // cast: see encodeConfig()
            break;
        case SensorType::Sds011:
            o["rx"] = s.rx;
            o["tx"] = s.tx;
            break;
        case SensorType::Pulse:
            o["pin"]         = s.pin;
            o["mode"]        = pulseModeName(s.mode);
            o["per_pulse"]   = s.per_pulse;
            o["debounce_us"] = s.debounce_us;
            break;
        default:
            break;
    }
}

}  // namespace detail

/// Write `c` into `out` as the §1 document.
///
/// `net` is written only for a WiFi node, `link`, `batt` and `sleep` only for
/// an ESP-NOW node — so the same call produces the radio's CFG (no `net`, by
/// transport) and the WiFi node's reply (no `link`).
///
/// LINKED vs COPIED: every char-array member is cast to `const char*` before
/// it is assigned, and that cast is load-bearing. ArduinoJson 7.4 takes a
/// `const char (&)[N]` for a string LITERAL — it stores the pointer, not the
/// bytes, with length N-1 — and a member of a `const NodeConfig&` has exactly
/// that type. Uncast, `name` would be serialised as all 16 bytes of its array,
/// NUL and stale bytes included, and would dangle once `c` went away. A
/// `const char*` is copied into the document, so `out` owns its strings.
static inline void encodeConfig(const NodeConfig& c, JsonObject out, uint8_t flags) {
    const bool secrets = (flags & NCJ_SECRETS) != 0;
    const bool espnow  = (c.transport == Transport::EspNow);

    out["rev"]        = c.rev;
    out["local"]      = c.local;
    out["transport"]  = transportName(c.transport);
    out["hw"]         = hwName(c.hw);
    out["fw"]         = (const char*)c.fw;
    out["name"]       = (const char*)c.name;
    out["interval_s"] = c.interval_s;
    out["altitude_m"] = c.altitude_m;
    out["board"]      = c.board;
    if (espnow) out["sleep"] = c.sleep;

    JsonObject i2c = out["i2c"].to<JsonObject>();
    i2c["sda"] = c.i2c.sda;
    i2c["scl"] = c.i2c.scl;

    JsonArray sensors = out["sensors"].to<JsonArray>();
    const uint8_t k = c.sensor_count <= MAX_SENSORS ? c.sensor_count : MAX_SENSORS;
    for (uint8_t i = 0; i < k; i++)
        detail::encodeSensor(c.sensors[i], sensors.add<JsonObject>());

    if (!espnow) {
        JsonObject n = out["net"].to<JsonObject>();
        n["ssid"] = (const char*)c.net.ssid;
        detail::putSecret(n, "pass", "pass_set", c.net.pass, secrets);
        n["host"] = (const char*)c.net.host;
        n["port"] = c.net.port;
        detail::putSecret(n, "token", "token_set", c.net.token, secrets);
        n["basic_user"] = (const char*)c.net.basic_user;
        detail::putSecret(n, "basic_pass", "basic_pass_set", c.net.basic_pass, secrets);
        JsonObject nx = n["next"].to<JsonObject>();
        nx["ssid"] = (const char*)c.net.next.ssid;
        detail::putSecret(nx, "pass", "pass_set", c.net.next.pass, secrets);
    } else {
        JsonObject l = out["link"].to<JsonObject>();
        l["ack_window_ms"] = c.link.ack_window_ms;
        l["rescan_fails"]  = c.link.rescan_fails;
        l["rescan_min_s"]  = c.link.rescan_min_s;
        l["next_ssid"]     = (const char*)c.link.next_ssid;
        JsonObject b = out["batt"].to<JsonObject>();
        b["pin"]     = c.batt.pin;
        b["divider"] = c.batt.divider;
        b["trim"]    = c.batt.trim;
        if (flags & NCJ_LMK) detail::putSecret(out, "lmk", "lmk_set", c.lmk, false);
    }
}

/// Encode `c` as compact JSON into `buf`. Returns the length written (no
/// terminator counted; one is written), or 0 when it does not fit — never a
/// truncated document. This is what a CFG / CFG_REPORT slices up.
static inline size_t encodeConfigTo(const NodeConfig& c, char* buf, size_t cap, uint8_t flags) {
    if (!buf || cap == 0) return 0;
    JsonDocument doc;
    encodeConfig(c, doc.to<JsonObject>(), flags);
    if (doc.overflowed()) return 0;
    const size_t need = measureJson(doc);
    if (need + 1 > cap) return 0;
    return serializeJson(doc, buf, cap);
}

/// A validation outcome as the page and the collector's API return it:
/// {"ok":true} or {"ok":false,"field":…,"reason":…}, plus "warnings":
/// [{"field","reason"}…] when there are any.
static inline void encodeValidation(const Validation& v, JsonObject out) {
    out["ok"] = v.ok;
    if (!v.ok) {
        out["field"]  = (const char*)v.error.field;
        out["reason"] = (const char*)v.error.reason;
    }
    if (v.warnCount) {
        JsonArray w = out["warnings"].to<JsonArray>();
        for (uint8_t i = 0; i < v.warnCount && i < MAX_WARNINGS; i++) {
            JsonObject o = w.add<JsonObject>();
            o["field"]  = (const char*)v.warnings[i].field;
            o["reason"] = (const char*)v.warnings[i].reason;
        }
    }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

namespace detail {

struct Dec {
    Issue* err;
    char   path[EN_CFG_FIELD_LEN];

    bool fail(const char* field, const char* reason) {
        if (err) {
            copyStr(err->field, sizeof(err->field), field);
            copyStr(err->reason, sizeof(err->reason), reason);
        }
        return false;
    }
    const char* at(const char* prefix, const char* key) {
        path[0] = '\0';
        if (prefix && prefix[0]) {
            strAppend(path, sizeof(path), prefix);
            strAppend(path, sizeof(path), ".");
        }
        strAppend(path, sizeof(path), key);
        return path;
    }
};

/// Read an integer in lo..hi into `dst` if `v` is present. A present value
/// of the wrong type, or out of range for the field, is an error — never a
/// silent wrap: a pin of 262 must not become GPIO6.
template <typename T>
static inline bool readInt(JsonVariantConst v, long lo, long hi, T& dst, Dec& d,
                           const char* field) {
    if (v.isNull()) return true;
    if (!v.is<long>()) {
        if (v.is<float>()) return d.fail(field, "must be a whole number");
        return d.fail(field, "must be a number");
    }
    const long x = v.as<long>();
    if (x < lo || x > hi) {
        char r[EN_CFG_REASON_LEN];
        r[0] = '\0';
        strAppend(r, sizeof(r), "must be ");
        strAppendUint(r, sizeof(r), (unsigned long)lo);   // every range here starts at 0
        strAppend(r, sizeof(r), "..");
        strAppendUint(r, sizeof(r), (unsigned long)hi);
        return d.fail(field, r);
    }
    dst = (T)x;
    return true;
}

static inline bool readFloat(JsonVariantConst v, float& dst, Dec& d, const char* field) {
    if (v.isNull()) return true;
    if (!v.is<float>()) return d.fail(field, "must be a number");
    dst = v.as<float>();
    return true;
}

static inline bool readBool(JsonVariantConst v, bool& dst, Dec& d, const char* field) {
    if (v.isNull()) return true;
    if (!v.is<bool>()) return d.fail(field, "must be true or false");
    dst = v.as<bool>();
    return true;
}

static inline bool readStr(JsonVariantConst v, char* dst, size_t cap, Dec& d,
                           const char* field) {
    if (v.isNull()) return true;
    if (!v.is<const char*>()) return d.fail(field, "must be a string");
    const char* s = v.as<const char*>();
    if (strlen(s) >= cap) {
        char r[EN_CFG_REASON_LEN];
        r[0] = '\0';
        strAppend(r, sizeof(r), "at most ");
        strAppendUint(r, sizeof(r), (unsigned long)(cap - 1));
        strAppend(r, sizeof(r), " characters");
        return d.fail(field, r);
    }
    copyStr(dst, cap, s);
    return true;
}

/// The write-only rule: non-empty sets, "" keeps, "" with <key>_set:false
/// clears. See the header comment.
static inline bool readSecret(JsonObjectConst o, const char* key, const char* setKey,
                              char* dst, size_t cap, Dec& d, const char* field) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<const char*>()) return d.fail(field, "must be a string");
    const char* s = v.as<const char*>();
    if (s[0] == '\0') {
        JsonVariantConst flag = o[setKey];
        if (flag.is<bool>() && !flag.as<bool>()) dst[0] = '\0';
        return true;
    }
    return readStr(v, dst, cap, d, field);
}

static inline bool decodeSensor(JsonVariantConst in, uint8_t idx, const SensorCfg* prev,
                                Hw hw, SensorCfg& out, Dec& d) {
    char pre[EN_CFG_FIELD_LEN];
    sensorFieldPath(pre, sizeof(pre), idx, nullptr);
    if (!in.is<JsonObjectConst>()) return d.fail(pre, "must be an object");
    JsonObjectConst o = in.as<JsonObjectConst>();

    JsonVariantConst tv = o["type"];
    SensorType type;
    if (!tv.is<const char*>() || !parseSensorType(tv.as<const char*>(), type))
        return d.fail(d.at(pre, "type"), "unknown sensor type");

    // An entry that kept its place and its type keeps what it does not
    // mention; anything else starts from the type's defaults for this chip.
    out = (prev && prev->type == type) ? *prev : sensorDefaults(type, hw);

    switch (type) {
        case SensorType::Bmx280:
        case SensorType::Bme688:
        case SensorType::Bh1750:
            return readInt(o["addr"], 0, 127, out.addr, d, d.at(pre, "addr"));
        case SensorType::Ds18b20:
            return readInt(o["pin"], 0, 255, out.pin, d, d.at(pre, "pin")) &&
                   readInt(o["count"], 0, 255, out.count, d, d.at(pre, "count")) &&
                   readStr(o["metric"], out.metric, sizeof(out.metric), d,
                           d.at(pre, "metric"));
        case SensorType::Sds011:
            return readInt(o["rx"], 0, 255, out.rx, d, d.at(pre, "rx")) &&
                   readInt(o["tx"], 0, 255, out.tx, d, d.at(pre, "tx"));
        case SensorType::Pulse: {
            JsonVariantConst mv = o["mode"];
            if (!mv.isNull()) {
                PulseMode m;
                if (!mv.is<const char*>() || !parsePulseMode(mv.as<const char*>(), m))
                    return d.fail(d.at(pre, "mode"), "must be rain or flow");
                // A change of mode brings that mode's scale and debounce
                // unless the document gives them: a flow meter inheriting a
                // rain gauge's 10 ms debounce would read a fraction of the
                // real flow and look perfectly healthy.
                if (m != out.mode) pulseModeDefaults(out, m);
            }
            return readInt(o["pin"], 0, 255, out.pin, d, d.at(pre, "pin")) &&
                   readFloat(o["per_pulse"], out.per_pulse, d, d.at(pre, "per_pulse")) &&
                   readInt(o["debounce_us"], 0, 2147483647L, out.debounce_us, d,
                           d.at(pre, "debounce_us"));
        }
        default:
            return true;
    }
}

static inline bool decodeInto(JsonObjectConst o, NodeConfig& c, uint8_t flags, Dec& d) {
    if (flags & NCJ_DEC_REV) {
        if (!readInt(o["rev"], 0, 65535, c.rev, d, "rev")) return false;
        if (!readBool(o["local"], c.local, d, "local")) return false;
    }
    if (flags & NCJ_DEC_IDENTITY) {
        JsonVariantConst t = o["transport"];
        if (!t.isNull() && (!t.is<const char*>() || !parseTransport(t.as<const char*>(), c.transport)))
            return d.fail("transport", "must be wifi or espnow");
        JsonVariantConst h = o["hw"];
        if (!h.isNull() && (!h.is<const char*>() || !parseHw(h.as<const char*>(), c.hw)))
            return d.fail("hw", "must be esp8266 or esp32c3");
        if (!readStr(o["fw"], c.fw, sizeof(c.fw), d, "fw")) return false;
    }

    if (!readStr(o["name"], c.name, sizeof(c.name), d, "name")) return false;
    if (!readInt(o["interval_s"], 0, 65535, c.interval_s, d, "interval_s")) return false;
    if (!readFloat(o["altitude_m"], c.altitude_m, d, "altitude_m")) return false;
    if (!readInt(o["board"], 0, 255, c.board, d, "board")) return false;
    if (!readBool(o["sleep"], c.sleep, d, "sleep")) return false;

    JsonVariantConst i2c = o["i2c"];
    if (!i2c.isNull()) {
        if (!i2c.is<JsonObjectConst>()) return d.fail("i2c", "must be an object");
        if (!readInt(i2c["sda"], 0, 255, c.i2c.sda, d, "i2c.sda")) return false;
        if (!readInt(i2c["scl"], 0, 255, c.i2c.scl, d, "i2c.scl")) return false;
    }

    JsonVariantConst sv = o["sensors"];
    if (!sv.isNull()) {
        if (!sv.is<JsonArrayConst>()) return d.fail("sensors", "must be an array");
        JsonArrayConst arr = sv.as<JsonArrayConst>();
        if (arr.size() > MAX_SENSORS) return d.fail("sensors", "at most 8 sensors");
        SensorCfg next[MAX_SENSORS];
        uint8_t n = 0;
        for (JsonVariantConst e : arr) {
            const SensorCfg* prev = n < c.sensor_count ? &c.sensors[n] : nullptr;
            if (!decodeSensor(e, n, prev, c.hw, next[n], d)) return false;
            n++;
        }
        for (uint8_t i = 0; i < MAX_SENSORS; i++) c.sensors[i] = i < n ? next[i] : SensorCfg();
        c.sensor_count = n;
    }

    // Transport-specific sections are read only for the transport they
    // belong to, and otherwise ignored like any unknown key: an ESP-NOW node
    // has no use for a `net`, and must not store one if a page sends it.
    if (c.transport == Transport::Wifi) {
        JsonVariantConst nv = o["net"];
        if (!nv.isNull()) {
            if (!nv.is<JsonObjectConst>()) return d.fail("net", "must be an object");
            JsonObjectConst n = nv.as<JsonObjectConst>();
            NetCfg& t = c.net;
            if (!readStr(n["ssid"], t.ssid, sizeof(t.ssid), d, "net.ssid") ||
                !readSecret(n, "pass", "pass_set", t.pass, sizeof(t.pass), d, "net.pass") ||
                !readStr(n["host"], t.host, sizeof(t.host), d, "net.host") ||
                !readInt(n["port"], 0, 65535, t.port, d, "net.port") ||
                !readSecret(n, "token", "token_set", t.token, sizeof(t.token), d, "net.token") ||
                !readStr(n["basic_user"], t.basic_user, sizeof(t.basic_user), d,
                         "net.basic_user") ||
                !readSecret(n, "basic_pass", "basic_pass_set", t.basic_pass,
                            sizeof(t.basic_pass), d, "net.basic_pass"))
                return false;
            JsonVariantConst xv = n["next"];
            if (!xv.isNull()) {
                if (!xv.is<JsonObjectConst>()) return d.fail("net.next", "must be an object");
                JsonObjectConst x = xv.as<JsonObjectConst>();
                if (!readStr(x["ssid"], t.next.ssid, sizeof(t.next.ssid), d, "net.next.ssid") ||
                    !readSecret(x, "pass", "pass_set", t.next.pass, sizeof(t.next.pass), d,
                                "net.next.pass"))
                    return false;
            }
        }
    } else {
        JsonVariantConst lv = o["link"];
        if (!lv.isNull()) {
            if (!lv.is<JsonObjectConst>()) return d.fail("link", "must be an object");
            if (!readInt(lv["ack_window_ms"], 0, 65535, c.link.ack_window_ms, d,
                         "link.ack_window_ms") ||
                !readInt(lv["rescan_fails"], 0, 255, c.link.rescan_fails, d,
                         "link.rescan_fails") ||
                !readInt(lv["rescan_min_s"], 0, 604800L, c.link.rescan_min_s, d,
                         "link.rescan_min_s") ||
                !readStr(lv["next_ssid"], c.link.next_ssid, sizeof(c.link.next_ssid), d,
                         "link.next_ssid"))
                return false;
        }
        JsonVariantConst bv = o["batt"];
        if (!bv.isNull()) {
            if (!bv.is<JsonObjectConst>()) return d.fail("batt", "must be an object");
            if (!readInt(bv["pin"], 0, 255, c.batt.pin, d, "batt.pin") ||
                !readFloat(bv["divider"], c.batt.divider, d, "batt.divider") ||
                !readFloat(bv["trim"], c.batt.trim, d, "batt.trim"))
                return false;
        }
        if (flags & NCJ_DEC_LMK) {
            JsonVariantConst kv = o["lmk"];
            if (!kv.isNull()) {
                if (!kv.is<const char*>()) return d.fail("lmk", "must be a string");
                const size_t kl = strlen(kv.as<const char*>());
                if (kl != 0 && kl != LMK_LEN)
                    return d.fail("lmk", "must be exactly 16 characters");
                if (!readSecret(o, "lmk", "lmk_set", c.lmk, sizeof(c.lmk), d, "lmk"))
                    return false;
            }
        }
    }
    return true;
}

}  // namespace detail

/// Apply the §1 document `in` on top of `cfg`.
///
/// Missing keys keep their current values; unknown keys are ignored; a value
/// of the wrong type or out of its field's range is an error, reported in
/// `err` (may be null) with the same field paths the validator uses. On an
/// error `cfg` is left exactly as it was.
///
/// This checks SHAPE, not sense: GPIO6 decodes fine and is then refused by
/// validate(). Every caller runs both, decode first.
///
/// Works on a copy of `cfg` (about 900 bytes) so a failure cannot leave it
/// half-applied; on the ESP8266 keep `cfg` itself off the stack if the caller
/// is already deep.
static inline bool decodeConfig(JsonVariantConst in, NodeConfig& cfg, uint8_t flags,
                                Issue* err) {
    detail::Dec d;
    d.err = err;
    d.path[0] = '\0';
    if (err) { err->field[0] = '\0'; err->reason[0] = '\0'; }
    if (!in.is<JsonObjectConst>()) return d.fail("", "the config must be a JSON object");
    NodeConfig tmp = cfg;
    if (!detail::decodeInto(in.as<JsonObjectConst>(), tmp, flags, d)) return false;
    cfg = tmp;
    return true;
}

// ---------------------------------------------------------------------------
// The old flat /config.json (node/src/NodeSettings.cpp before this format)
// ---------------------------------------------------------------------------

/// Does this look like the WiFi node's pre-§1 flat config? True for an object
/// with any of the old top-level keys and none of the new sections — the old
/// format never had `net`, `sensors` or `rev`.
static inline bool isLegacyWifiDoc(JsonVariantConst in) {
    if (!in.is<JsonObjectConst>()) return false;
    JsonObjectConst o = in.as<JsonObjectConst>();
    if (!o["net"].isNull() || !o["sensors"].isNull() || !o["rev"].isNull()) return false;
    static const char* const OLD[] = { "ssid", "host", "nodeId", "intervalMs", "i2cSda",
                                       "token", "port" };
    for (size_t i = 0; i < sizeof(OLD) / sizeof(OLD[0]); i++)
        if (!o[OLD[i]].isNull()) return true;
    return false;
}

/// Carry an old flat /config.json into `cfg`.
///
/// `cfg` must already hold the new defaults AND the sensor list the firmware
/// was built with: the old file never said which sensors a node had (that was
/// NODE_SENSOR_* at compile time), only where they were wired. So this moves
/// the pins onto whichever entries exist — oneWirePin to every ds18b20 entry,
/// pulsePin to the pulse entry, sdsRx/sdsTx to the sds011 — and leaves the
/// list itself alone.
///
/// Forgiving where decodeConfig() is strict, because there is nobody to show
/// an error to: the node is booting. A value that does not fit keeps the
/// default; strings are cut to fit (every old buffer was the same size or
/// smaller, so in practice nothing is). The name is taken as it was even if
/// it would not pass validate() today: renaming it would make the collector
/// see a new node and orphan the old one's history, so the user gets to see
/// the complaint on the page and choose.
///
/// Returns false if `in` is not an object.
static inline bool migrateLegacyWifi(JsonVariantConst in, NodeConfig& cfg) {
    if (!in.is<JsonObjectConst>()) return false;
    JsonObjectConst o = in.as<JsonObjectConst>();

    struct Str {
        static void take(JsonVariantConst v, char* dst, size_t cap) {
            if (v.is<const char*>()) copyStr(dst, cap, v.as<const char*>());
        }
    };
    struct Num {
        static bool take(JsonVariantConst v, long lo, long hi, long& out) {
            if (!v.is<long>()) return false;
            const long x = v.as<long>();
            if (x < lo || x > hi) return false;
            out = x;
            return true;
        }
    };

    Str::take(o["ssid"],      cfg.net.ssid,       sizeof(cfg.net.ssid));
    Str::take(o["pass"],      cfg.net.pass,       sizeof(cfg.net.pass));
    Str::take(o["host"],      cfg.net.host,       sizeof(cfg.net.host));
    Str::take(o["token"],     cfg.net.token,      sizeof(cfg.net.token));
    Str::take(o["nodeId"],    cfg.name,           sizeof(cfg.name));
    Str::take(o["basicUser"], cfg.net.basic_user, sizeof(cfg.net.basic_user));
    Str::take(o["basicPass"], cfg.net.basic_pass, sizeof(cfg.net.basic_pass));

    long x;
    if (Num::take(o["port"], 1, 65535, x)) cfg.net.port = (uint16_t)x;
    if (Num::take(o["intervalMs"], 0, 2147483647L, x)) {
        // Milliseconds to seconds, rounded, and pulled into the range the new
        // format allows: an old node posting every 5 s becomes 10 s rather
        // than a config its own validator refuses. Rounded without adding
        // first: x + 500 overflows a 32-bit long near its top.
        long s = x / 1000 + (x % 1000 >= 500);
        if (s < (long)INTERVAL_MIN_S) s = INTERVAL_MIN_S;
        if (s > (long)INTERVAL_MAX_S) s = INTERVAL_MAX_S;
        cfg.interval_s = (uint16_t)s;
    }
    JsonVariantConst alt = o["altitudeM"];
    if (alt.is<float>()) cfg.altitude_m = alt.as<float>();
    if (Num::take(o["board"], 0, 255, x)) cfg.board = (uint8_t)x;
    if (Num::take(o["i2cSda"], 0, 255, x)) cfg.i2c.sda = (uint8_t)x;
    if (Num::take(o["i2cScl"], 0, 255, x)) cfg.i2c.scl = (uint8_t)x;

    long ow = -1, pp = -1, rx = -1, tx = -1;
    Num::take(o["oneWirePin"], 0, 255, ow);
    Num::take(o["pulsePin"],   0, 255, pp);
    Num::take(o["sdsRx"],      0, 255, rx);
    Num::take(o["sdsTx"],      0, 255, tx);
    for (uint8_t i = 0; i < cfg.sensor_count && i < MAX_SENSORS; i++) {
        SensorCfg& s = cfg.sensors[i];
        if (s.type == SensorType::Ds18b20 && ow >= 0) s.pin = (uint8_t)ow;
        if (s.type == SensorType::Pulse   && pp >= 0) s.pin = (uint8_t)pp;
        if (s.type == SensorType::Sds011) {
            if (rx >= 0) s.rx = (uint8_t)rx;
            if (tx >= 0) s.tx = (uint8_t)tx;
        }
    }

    cfg.transport = Transport::Wifi;
    cfg.rev   = 0;       // never synced: the collector has not seen this format
    cfg.local = false;
    return true;
}

// ---------------------------------------------------------------------------
// caps — what the page may offer on this node (docs/NODE_CONFIG.md §6)
// ---------------------------------------------------------------------------

namespace detail {

/// "A0,RSV,D0" -> ["A0","RSV","D0"]. The pads are copied (the source strings
/// are static, but a copy keeps the document self-contained).
static inline void putPads(JsonArray arr, const char* list) {
    if (!list) return;
    char pad[16];
    size_t n = 0;
    for (const char* p = list;; p++) {
        if (*p == ',' || *p == '\0') {
            pad[n] = '\0';
            if (n) arr.add((const char*)pad);   // const char*: copied
            n = 0;
            if (*p == '\0') break;
        } else if (n < sizeof(pad) - 1) {
            pad[n++] = *p;
        }
    }
}

}  // namespace detail

/// Write the §6 `caps` object for a node of this transport and chip.
///
/// Beyond §6: `sleep_unsafe` (the types §1.2 refuses on a sleeping ESP-NOW
/// node), `metric_count` (budget cost per type; ds18b20's is per probe),
/// `max_gpio`, and per-board `left` / `right` header pad lists — so the page
/// can check the budget and draw the board without a copy of these tables in
/// JavaScript.
static inline void encodeCaps(Transport t, Hw hw, JsonObject out) {
    const HwInfo& h = hwInfo(hw);
    out["transport"] = transportName(t);
    out["hw"]        = hwName(hw);

    JsonArray types = out["sensor_types"].to<JsonArray>();
    JsonArray unsafe = out["sleep_unsafe"].to<JsonArray>();
    JsonObject mc = out["metric_count"].to<JsonObject>();
    for (uint8_t i = 1; i < SENSOR_TYPE_COUNT; i++) {
        const SensorType st = (SensorType)i;
        types.add(sensorTypeName(st));
        if (sensorNeedsAwake(st)) unsafe.add(sensorTypeName(st));
        SensorCfg probe = sensorDefaults(st, hw);
        probe.count = 1;
        mc[sensorTypeName(st)] = sensorMetricCount(probe);
    }

    JsonArray boards = out["boards"].to<JsonArray>();
    for (uint8_t b = 0; b < h.boardCount; b++) {
        const BoardInfo& bi = h.boards[b];
        JsonObject o = boards.add<JsonObject>();
        o["id"]   = bi.id;
        o["name"] = bi.name;
        JsonObject pins = o["pins"].to<JsonObject>();
        for (uint8_t i = 0; i < bi.labelCount; i++) pins[bi.labels[i].label] = bi.labels[i].gpio;
        if (bi.left && bi.left[0])   detail::putPads(o["left"].to<JsonArray>(), bi.left);
        if (bi.right && bi.right[0]) detail::putPads(o["right"].to<JsonArray>(), bi.right);
    }

    JsonArray forb = out["forbidden_pins"].to<JsonArray>();
    JsonObject warn = out["warn_pins"].to<JsonObject>();
    for (uint8_t i = 0; i < h.noteCount; i++) {
        const PinNote& n = h.notes[i];
        if (n.risk == PIN_FORBIDDEN) {
            forb.add(n.gpio);
        } else if (n.risk == PIN_WARN) {
            char key[4];
            key[0] = '\0';
            strAppendUint(key, sizeof(key), n.gpio);
            warn[(const char*)key] = n.why;
        }
    }
    out["max_gpio"]    = h.maxGpio;
    out["max_sensors"] = MAX_SENSORS;
    out["max_metrics"] = MAX_METRICS;
}

}  // namespace nodecfg
