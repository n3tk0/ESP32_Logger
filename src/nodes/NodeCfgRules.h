// ============================================================================
// src/nodes/NodeCfgRules.h
//
// The collector's decisions about node configuration, as pure functions.
//
// docs/NODE_CONFIG.md is the contract. The collector side of it is mostly
// plumbing — files, radio frames, HTTP — and a handful of decisions that are
// easy to get subtly wrong and impossible to see on a bench:
//
//   * what a report from a node does to the desired config (§0.4, §3, §5);
//   * when a node is sent a config, and when it is left alone;
//   * which secrets travel in a reply, and which are sent as "keep yours";
//   * whether a node is ready, pending or offline in a network handover (§4);
//   * what a DATA2 value is called (§5).
//
// They live here, with no Arduino, no filesystem and no radio, so that
// tests/host/test_nodecfg_collector.cpp can walk every branch. The firmware
// (NodeCfgStore.cpp, EspNowIngest.cpp, IngestHandler.cpp) only moves bytes
// between these functions and the outside world.
//
// C++11, like everything the collector compiles.
// ============================================================================
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

#include "../nodecfg/NodeConfig.h"
#include "../nodecfg/MetricCatalog.h"
#include "../nodecfg/NodeConfigValidate.h"   // Issue
#include "../espnow/EspNowProto.h"

namespace ncr {

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/// Per-node config status, as /api/nodes/config and the status lists name it.
/// ST_NONE is "no config file" and is never written to one.
enum : uint8_t { ST_NONE = 0, ST_APPLIED = 1, ST_PENDING = 2, ST_REJECTED = 3 };

static inline const char* statusName(uint8_t s) {
    switch (s) {
        case ST_APPLIED:  return "applied";
        case ST_PENDING:  return "pending";
        case ST_REJECTED: return "rejected";
        default:          return nullptr;
    }
}

static inline uint8_t statusParse(const char* s) {
    if (!s) return ST_NONE;
    if (strcmp(s, "applied") == 0)  return ST_APPLIED;
    if (strcmp(s, "pending") == 0)  return ST_PENDING;
    if (strcmp(s, "rejected") == 0) return ST_REJECTED;
    return ST_NONE;
}

/// The next revision. Wraps to 1, never to 0 — 0 means "never synced".
static inline uint16_t nextRev(uint16_t rev) {
    return rev == 0xFFFF ? 1 : (uint16_t)(rev + 1);
}

/// Is `a` the same rev as `b`, or a later one? Revs are compared on a
/// circle, not as plain numbers: nextRev() wraps 0xFFFF to 1, and after that
/// a plain `>=` calls rev 1 older than 65535 — the node would be called up to
/// date and never sent it. The difference read as signed is right while the
/// two are less than 32768 revs apart. 0 ("never synced") is behind every rev.
static inline bool revAtOrPast(uint16_t a, uint16_t b) {
    if (!b) return true;
    if (!a) return false;
    return (int16_t)(uint16_t)(a - b) >= 0;
}

/// What the status becomes once the node says it runs `applied`.
///
/// A node that caught up is applied, whatever it was. One that is still
/// behind keeps its status: pending stays pending, and rejected stays
/// rejected — the rejected rev is still the desired one, and the node is
/// still (correctly) running its previous config.
static inline uint8_t statusAfterApplied(uint8_t status, uint16_t rev, uint16_t applied) {
    return revAtOrPast(applied, rev) ? (uint8_t)ST_APPLIED : status;
}

/// Is there a config the node should be handed? Pending and behind. A
/// rejected rev is NOT re-sent: the node would refuse it again on every
/// contact, and on an ESP-NOW node every refusal is battery.
static inline bool shouldSend(uint8_t status, uint16_t rev, uint16_t applied) {
    return status == ST_PENDING && !revAtOrPast(applied, rev);
}

// ---------------------------------------------------------------------------
// Keys and file names
// ---------------------------------------------------------------------------
// "w:<name>" for a WiFi node (the name it posts to /api/ingest as), "e:<id>"
// for an ESP-NOW node (its 1..254 radio id). Files: /nodes/w_<name>.json and
// /nodes/e_<id>.json (docs/NODE_CONFIG.md §2).

static const size_t KEY_CAP  = 20;   ///< "w:" + 16 + NUL
static const size_t PATH_CAP = 32;   ///< "/nodes/w_" + 16 + ".json" + NUL = 31

/// A §1 name: 1..16 of [A-Za-z0-9_-]. Also what makes a WiFi node's id safe
/// to use as a file name — an ingest id that fails this gets no config file.
static inline bool validName(const char* s) {
    if (!s || !*s) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= nodecfg::NODE_NAME_MAX) return false;
    }
    return true;
}

/// Parse "w:balcony" / "e:3". `name` gets the WiFi name; `id` the ESP-NOW id.
static inline bool parseKey(const char* key, bool& espnow, char name[nodecfg::NODE_NAME_MAX + 1],
                            uint8_t& id) {
    if (!key || !key[0] || key[1] != ':') return false;
    name[0] = '\0';
    id = 0;
    if (key[0] == 'w') {
        if (!validName(key + 2)) return false;
        espnow = false;
        nodecfg::copyStr(name, nodecfg::NODE_NAME_MAX + 1, key + 2);
        return true;
    }
    if (key[0] != 'e') return false;
    unsigned v = 0;
    const char* p = key + 2;
    if (!*p || (*p == '0')) return false;              // no leading zeros, no empty
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 254) return false;
    }
    espnow = true;
    id = (uint8_t)v;
    return true;
}

static inline void formatKey(char out[KEY_CAP], bool espnow, const char* name, uint8_t id) {
    out[0] = espnow ? 'e' : 'w';
    out[1] = ':';
    out[2] = '\0';
    if (espnow) nodecfg::strAppendUint(out, KEY_CAP, id);
    else        nodecfg::strAppend(out, KEY_CAP, name);
}

static inline void filePath(char out[PATH_CAP], bool espnow, const char* name, uint8_t id) {
    out[0] = '\0';
    nodecfg::strAppend(out, PATH_CAP, espnow ? "/nodes/e_" : "/nodes/w_");
    if (espnow) nodecfg::strAppendUint(out, PATH_CAP, id);
    else        nodecfg::strAppend(out, PATH_CAP, name);
    nodecfg::strAppend(out, PATH_CAP, ".json");
}

// ---------------------------------------------------------------------------
// A node reporting its own config (§0.4, §3, §5 CFG_REPORT)
// ---------------------------------------------------------------------------

/// What a report does.
///
///   adopt    the report becomes the desired config at `rev`
///   rev      the desired rev afterwards
///   applied  what the node now runs — always the rev it is told (or has)
///
/// The rules:
///  * No config held yet: adopt. A node that has never been synced (rev 0)
///    starts at 1 (§0.2); one that has (the collector lost its file) keeps its
///    number, so a pending change on either side is not mistaken for none —
///    unless it is a local edit, which is one past what it had.
///  * A local edit wins (§0.4): adopted at one past the larger of the two
///    revs, so it supersedes whatever the collector had pending.
///  * A plain report (first contact after boot) from a node that is AHEAD of
///    the collector — a restored backup, a reflashed collector — is adopted
///    at the node's rev: the collector cannot hold a config older than the
///    one the node says it came from.
///  * Otherwise it is a status report: nothing adopted, applied = its rev. A
///    node behind the desired config (including one reset to rev 0) is then
///    simply pending, and gets the desired config back.
struct ReportPlan {
    bool     adopt;
    uint16_t rev;
    uint16_t applied;
};

static inline ReportPlan planReport(bool have, uint16_t desiredRev, uint16_t reportRev, bool local) {
    ReportPlan p;
    p.adopt = true;
    if (!have) {
        p.rev = local ? nextRev(reportRev) : (reportRev ? reportRev : 1);
    } else if (local) {
        p.rev = nextRev(revAtOrPast(desiredRev, reportRev) ? desiredRev : reportRev);
    } else if (!revAtOrPast(desiredRev, reportRev)) {
        p.rev = reportRev;
    } else {
        p.adopt   = false;
        p.rev     = desiredRev;
        p.applied = reportRev;
        return p;
    }
    p.applied = p.rev;
    return p;
}

/// What the WiFi node's ingest reply carries (§3).
enum : uint8_t { REPLY_NONE = 0, REPLY_REV = 1, REPLY_FULL = 2 };

/// `adopted`: this very request's report was adopted — say its new rev.
/// Otherwise the full config goes out when the node is behind a pending
/// desired rev, and not for a rev the node has just refused (`refusedRev`,
/// the cfg_error's rev, 0 if none): §3's "no cfg_error for that rev".
static inline uint8_t ingestReply(bool adopted, uint8_t status, uint16_t rev,
                                  uint16_t nodeRev, uint16_t refusedRev) {
    if (adopted) return REPLY_REV;
    if (refusedRev && refusedRev == rev) return REPLY_NONE;
    return shouldSend(status, rev, nodeRev) ? REPLY_FULL : REPLY_NONE;
}

// ---------------------------------------------------------------------------
// Secrets (§0.5) — which ones the collector actually knows
// ---------------------------------------------------------------------------
// A node never sends its secrets; it sends "" + "<key>_set". So a secret the
// node set on its own page (its WiFi passphrase, typed into the setup portal)
// is one the collector has NEVER seen, and "keeping the stored one" keeps
// nothing, or keeps a stale one. Sending that stored value back in a reply
// would clear the node's passphrase, or roll it back.
//
// So the collector tracks two bits per secret:
//   known  the node has this secret set (from its report's *_set flags), or
//          the collector set it — what a GET reports as <key>_set;
//   dirty  the collector itself changed the value and the node has not yet
//          applied a rev carrying it — the only case in which the value is
//          sent. Every other secret goes out as "" + "<key>_set": true, which
//          the shared decoder reads as "keep yours".

enum : uint8_t { SEC_PASS = 1, SEC_TOKEN = 2, SEC_BPASS = 4, SEC_NPASS = 8 };

/// Bits of the secrets whose values differ between `a` and `b`.
static inline uint8_t secretsDiff(const nodecfg::NodeConfig& a, const nodecfg::NodeConfig& b) {
    uint8_t m = 0;
    if (strcmp(a.net.pass, b.net.pass) != 0)             m |= SEC_PASS;
    if (strcmp(a.net.token, b.net.token) != 0)           m |= SEC_TOKEN;
    if (strcmp(a.net.basic_pass, b.net.basic_pass) != 0) m |= SEC_BPASS;
    if (strcmp(a.net.next.pass, b.net.next.pass) != 0)   m |= SEC_NPASS;
    return m;
}

/// Bits of the secrets that are non-empty in `c`.
static inline uint8_t secretsSet(const nodecfg::NodeConfig& c) {
    uint8_t m = 0;
    if (c.net.pass[0])       m |= SEC_PASS;
    if (c.net.token[0])      m |= SEC_TOKEN;
    if (c.net.basic_pass[0]) m |= SEC_BPASS;
    if (c.net.next.pass[0])  m |= SEC_NPASS;
    return m;
}

/// After an edit that changed the bits in `changed`: those are now the
/// collector's to send, and known exactly when non-empty.
static inline void secretsEdited(uint8_t changed, const nodecfg::NodeConfig& now,
                                 uint8_t& known, uint8_t& dirty) {
    const uint8_t set = secretsSet(now);
    dirty |= changed;
    known = (uint8_t)((known & ~changed) | (set & changed));
}

namespace detail {
struct SecretSlot { uint8_t bit; const char* parent; const char* key; const char* setKey; };
static const SecretSlot SECRET_SLOTS[4] = {
    { SEC_PASS,  nullptr, "pass",       "pass_set" },
    { SEC_TOKEN, nullptr, "token",      "token_set" },
    { SEC_BPASS, nullptr, "basic_pass", "basic_pass_set" },
    { SEC_NPASS, "next",  "pass",       "pass_set" },
};
static inline JsonObject slotObj(JsonObject net, const SecretSlot& s) {
    return s.parent ? net[s.parent].as<JsonObject>() : net;
}
static inline JsonObjectConst slotObj(JsonObjectConst net, const SecretSlot& s) {
    return s.parent ? net[s.parent].as<JsonObjectConst>() : net;
}
}  // namespace detail

/// The *_set flags of a report's `net`, as bits. A missing flag counts as
/// not set; a report without `net` (ESP-NOW) has none.
static inline uint8_t reportSecretsKnown(JsonObjectConst doc) {
    JsonObjectConst net = doc["net"];
    if (net.isNull()) return 0;
    uint8_t m = 0;
    for (const detail::SecretSlot& s : detail::SECRET_SLOTS) {
        JsonObjectConst o = detail::slotObj(net, s);
        if (!o.isNull() && (o[s.setKey] | false)) m |= s.bit;
    }
    return m;
}

/// Rewrite an outgoing document written WITH secrets (encodeConfig +
/// NCJ_SECRETS) so that only the `dirty` ones carry a value; every other
/// secret becomes "" + "<key>_set": true — keep yours.
static inline void secretsForNode(JsonObject doc, uint8_t dirty) {
    JsonObject net = doc["net"];
    if (net.isNull()) return;
    for (const detail::SecretSlot& s : detail::SECRET_SLOTS) {
        if (dirty & s.bit) continue;
        JsonObject o = detail::slotObj(net, s);
        if (o.isNull()) continue;
        o[s.key]    = "";
        o[s.setKey] = true;
    }
}

/// Rewrite a GET document (written without secrets) so each *_set says
/// what the collector knows rather than whether its own copy is non-empty.
static inline void secretsForGet(JsonObject doc, uint8_t known) {
    JsonObject net = doc["net"];
    if (net.isNull()) return;
    for (const detail::SecretSlot& s : detail::SECRET_SLOTS) {
        JsonObject o = detail::slotObj(net, s);
        if (o.isNull()) continue;
        o[s.key]    = "";
        o[s.setKey] = (known & s.bit) != 0;
    }
}

// ---------------------------------------------------------------------------
// ESP-NOW identity on first contact
// ---------------------------------------------------------------------------

/// On the FIRST report from an ESP-NOW node, the collector's table label and
/// wake interval win over the node's: the label is the sensor id every
/// reading of that node is stored under, and a node upgraded from the fixed
/// firmware reports whatever default name it was built with. Adopting that
/// would move its readings to a new series. Returns true when `c` changed
/// (the caller then bumps the rev so the node is sent the collector's view).
/// A label that is not a valid §1 name is left alone — it cannot be sent.
static inline bool adoptTableIdentity(nodecfg::NodeConfig& c, const char* label, uint16_t intervalS) {
    bool changed = false;
    if (validName(label) && strcmp(c.name, label) != 0) {
        nodecfg::copyStr(c.name, sizeof(c.name), label);
        changed = true;
    }
    if (intervalS >= nodecfg::INTERVAL_MIN_S && c.interval_s != intervalS) {
        c.interval_s = intervalS;
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Handover (§4)
// ---------------------------------------------------------------------------

enum : uint8_t { HO_READY = 0, HO_PENDING = 1, HO_OFFLINE = 2 };

/// Ready when the node runs a rev at or past the one that carried the next
/// network — ready wins over offline, because a node that has what it needs
/// is ready whether or not it has spoken since. A node without a config file
/// cannot be handed anything and is never ready.
static inline uint8_t hoClassify(bool haveEntry, uint16_t applied, uint16_t hoRev, bool offline) {
    if (haveEntry && hoRev && revAtOrPast(applied, hoRev)) return HO_READY;
    return offline ? HO_OFFLINE : HO_PENDING;
}

/// Least time between start and an automatic switch, so the page that
/// started it sees at least one status before the collector goes away.
static const uint32_t HO_MIN_MS = 10000;

static inline bool hoAutoSwitch(bool active, int pending, uint32_t sinceStartMs) {
    return active && pending == 0 && sinceStartMs >= HO_MIN_MS;
}

/// Hand `c` the next network (`ssid` "" = take it back). WiFi: net.next,
/// with its passphrase. ESP-NOW: link.next_ssid — it never needs the pass.
/// Returns the secret bits that changed.
static inline uint8_t hoApplyNext(nodecfg::NodeConfig& c, const char* ssid, const char* pass) {
    if (c.transport == nodecfg::Transport::EspNow) {
        nodecfg::copyStr(c.link.next_ssid, sizeof(c.link.next_ssid), ssid);
        return 0;
    }
    const bool passChanged = strcmp(c.net.next.pass, pass ? pass : "") != 0;
    nodecfg::copyStr(c.net.next.ssid, sizeof(c.net.next.ssid), ssid);
    nodecfg::copyStr(c.net.next.pass, sizeof(c.net.next.pass), pass);
    return passChanged ? SEC_NPASS : 0;
}

// ---------------------------------------------------------------------------
// DATA2 naming (§5)
// ---------------------------------------------------------------------------

/// The part of a node's reported config DATA2 naming depends on — its
/// ds18b20 entries in list order — kept in RAM per ESP-NOW node instead of
/// the whole ~700-byte config.
struct ProbeMap {
    uint8_t n;                                             ///< ds18b20 entries
    uint8_t count[nodecfg::MAX_SENSORS];
    char    metric[nodecfg::MAX_SENSORS][nodecfg::DS_METRIC_MAX + 1];
};

static inline void probeMapFrom(const nodecfg::NodeConfig& c, ProbeMap& m) {
    memset(&m, 0, sizeof(m));
    const uint8_t k = c.sensor_count <= nodecfg::MAX_SENSORS ? c.sensor_count : nodecfg::MAX_SENSORS;
    for (uint8_t i = 0; i < k; i++) {
        const nodecfg::SensorCfg& s = c.sensors[i];
        if (s.type != nodecfg::SensorType::Ds18b20) continue;
        m.count[m.n] = s.count;
        nodecfg::copyStr(m.metric[m.n], sizeof(m.metric[m.n]), s.metric);
        m.n++;
    }
}

/// A config holding only the probe entries — which is all metricNameFor()
/// reads for a probe, and it walks them in the same order, so the names are
/// the ones the full config would give.
static inline void probeMapToConfig(const ProbeMap& m, nodecfg::NodeConfig& c) {
    c.sensor_count = 0;
    for (uint8_t i = 0; i < m.n && i < nodecfg::MAX_SENSORS; i++) {
        nodecfg::SensorCfg s;
        s.type  = nodecfg::SensorType::Ds18b20;
        s.count = m.count[i];
        nodecfg::copyStr(s.metric, sizeof(s.metric), m.metric[i]);
        c.sensors[c.sensor_count++] = s;
    }
}

struct NamedValue {
    char        name[nodecfg::METRIC_NAME_CAP];
    const char* unit;
    float       value;
};

/// One DATA2 sample as named readings. `cfg` is the node's reported config
/// (null before it has reported one). Unknown ids, probe indexes past the
/// config's probes and non-finite values are dropped (§5). `battV` is set to
/// the sample's battery_voltage, or left alone when it has none — the caller
/// feeds it to the battery model as well as storing it as a reading.
static inline int data2Named(const nodecfg::NodeConfig* cfg, const Data2Sample& s,
                             NamedValue* out, int max, float& battV) {
    int n = 0;
    for (uint8_t i = 0; i < s.n && i < EN_DATA2_MAX_VALUES; i++) {
        const Data2Value& v = s.v[i];
        const float x = v.value;
        if (!(x == x) || x - x != 0.0f) continue;          // NaN or ±inf
        if (n >= max) break;
        const char* unit = nodecfg::metricNameFor(cfg, v.metric, v.index, out[n].name);
        if (!unit) continue;
        out[n].unit  = unit;
        out[n].value = x;
        if (v.metric == nodecfg::M_BATTERY_VOLTAGE) battV = x;
        n++;
    }
    return n;
}

}  // namespace ncr
