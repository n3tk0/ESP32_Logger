#include "NodeCfgStore.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <LittleFS.h>
#include <new>
#include <time.h>

#include "../nodecfg/NodeConfigJson.h"
#include "../pipeline/DataPipeline.h"   // fsMutex
#include "../utils/AtomicWrite.h"
#include "../utils/MutexGuard.h"
#include "../sensors/RemoteIngest.h"   // the handover's WiFi node list
#include "../web/IngestHandler.h"       // REMOTE_STATUS_STALE_MS
#ifdef FEATURE_ESPNOW_INGEST
#include "../espnow/EspNowIngest.h"     // the table label follows the config
#endif

using namespace nodecfg;

// ============================================================================
// Index
// ============================================================================

struct Entry {
    bool           used;
    bool           espnow;
    uint8_t        id;            ///< ESP-NOW radio id
    bool           haveProbes;    ///< a reported config has been seen
    uint32_t       seenMs;        ///< WiFi: millis()|1 of its last POST; 0 = none since boot
    uint16_t       told;          ///< highest rev the node was told or ran (ncr::revAfter)
    char           name[NODE_NAME_MAX + 1];    ///< WiFi: the key
    char           dname[NODE_NAME_MAX + 1];   ///< desired name (a WiFi rename)
    char           sname[NODE_NAME_MAX + 1];   ///< WiFi: name in the last full reply
    NodeCfgSummary s;
    ncr::ProbeMap  probes;
};

static Entry             s_e[NODECFG_MAX_NODES];
static SemaphoreHandle_t s_mx  = nullptr;
static volatile uint32_t s_gen = 1;

static const char NODES_DIR[] = "/nodes";
static const char HO_PATH[]   = "/nodes/handover.json";

struct HandoverState {
    bool     active;
    uint32_t startMs;
    char     ssid[SSID_CAP];
};
static HandoverState s_ho;

/// The heap scratch every operation needs: a file's document, a config to
/// decode it into and the one it was before. ~2.2 KB plus the document,
/// which is too much for the async web task's stack alongside ArduinoJson.
struct Work {
    JsonDocument doc;
    JsonDocument aux;
    NodeConfig   cfg;
    NodeConfig   old;
    Validation   v;
    bool         hoKept;   ///< adopt() put the handover's next network back
};

/// Lock for every public entry point. Unlocked (and so a no-op caller) until
/// nodeCfgBegin() has created the mutex.
#define NC_LOCK(ms, fail)                                   \
    MutexGuard _g(s_mx, pdMS_TO_TICKS(ms));                 \
    if (!_g.isLocked()) return fail

static Entry* find(bool espnow, const char* name, uint8_t id) {
    for (Entry& e : s_e) {
        if (!e.used || e.espnow != espnow) continue;
        if (espnow ? e.id == id : strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

static Entry* alloc(bool espnow, const char* name, uint8_t id) {
    for (Entry& e : s_e) {
        if (e.used) continue;
        memset(&e, 0, sizeof(e));
        e.used   = true;
        e.espnow = espnow;
        e.id     = id;
        copyStr(e.name, sizeof(e.name), espnow ? "" : name);
        return &e;
    }
    Serial.println("[nodecfg] index full — config not kept");
    return nullptr;
}

// ============================================================================
// Files — every access under fsMutex (Pillar 1.3)
// ============================================================================

static bool readJson(const char* path, JsonDocument& doc) {
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err;
}

static bool writeJson(const char* path, const JsonDocument& doc) {
    return atomicWrite(LittleFS, path,
                       [&doc](File& f) { return serializeJson(doc, static_cast<Print&>(f)) > 0; }, fsMutex);
}

static void removeFile(const char* path) {
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return;
    LittleFS.remove(path);
}

/// Remember a rev the node was told, or says it runs, past the desired one.
/// True when it moved.
static bool noteRev(Entry& e, uint16_t rev) {
    if (!rev || ncr::revAtOrPast(e.told, rev)) return false;
    e.told = rev;
    return true;
}

static void pathOf(const Entry& e, char out[ncr::PATH_CAP]) {
    ncr::filePath(out, e.espnow, e.name, e.id);
}

static bool loadEntry(const Entry& e, JsonDocument& doc) {
    char p[ncr::PATH_CAP];
    pathOf(e, p);
    return readJson(p, doc) && doc["desired"].is<JsonObject>();
}

/// The `error` object of a rejected rev — {field, reason} — or null.
static void putError(JsonVariant dst, const NodeCfgSummary& s) {
    if (s.status != ncr::ST_REJECTED) { dst.set(nullptr); return; }
    JsonObject er = dst.to<JsonObject>();
    er["field"]  = (const char*)s.err.field;
    er["reason"] = (const char*)s.err.reason;
}

/// Write the RAM fields of `e` into `doc` and the file. The generation moves
/// even if the write fails: RAM is what everything else reads, and it holds
/// the new state either way.
static bool saveEntry(const Entry& e, JsonDocument& doc) {
    doc["applied_rev"] = e.s.applied;
    doc["status"]      = ncr::statusName(e.s.status);
    doc["ho_rev"]      = e.s.hoRev;
    doc["told_rev"]    = e.told;
    putError(doc["error"], e.s);
    const uint32_t now = (uint32_t)time(nullptr);
    if (now >= 1000000000u) doc["seen"] = now;
    s_gen++;
    char p[ncr::PATH_CAP];
    pathOf(e, p);
    if (writeJson(p, doc)) return true;
    Serial.printf("[nodecfg] could not write %s\n", p);
    return false;
}

static bool decodeDesired(JsonDocument& doc, NodeConfig& c, bool espnow) {
    c = configDefaults(espnow ? Transport::EspNow : Transport::Wifi,
                       espnow ? Hw::Esp32c3 : Hw::Esp8266);
    return decodeConfig(doc["desired"].as<JsonVariantConst>(), c,
                        NCJ_DEC_REV | NCJ_DEC_IDENTITY, nullptr);
}

static void encodeDesired(JsonDocument& doc, const NodeConfig& c) {
    encodeConfig(c, doc["desired"].to<JsonObject>(), NCJ_SECRETS);
}

/// A new desired rev: pending, no error, the new name remembered. Past any
/// rev the node already holds, not only the desired one (ncr::revAfter).
static void bumped(Entry& e, NodeConfig& c) {
    e.s.rev   = ncr::revAfter(e.s.rev, e.told);
    c.rev     = e.s.rev;
    c.local   = false;
    e.s.status = ncr::statusAfterApplied(ncr::ST_PENDING, e.s.rev, e.s.applied);
    e.s.err.field[0] = e.s.err.reason[0] = '\0';
    copyStr(e.dname, sizeof(e.dname), c.name);
}

/// The ds18b20 entries of a reported config, for DATA2 naming.
static void takeProbes(Entry& e, JsonVariantConst reported, NodeConfig& scratch) {
    if (!e.espnow || !reported.is<JsonObjectConst>()) return;
    scratch = configDefaults(Transport::EspNow, Hw::Esp32c3);
    if (!decodeConfig(reported, scratch, NCJ_DEC_REV | NCJ_DEC_IDENTITY, nullptr)) return;
    ncr::probeMapFrom(scratch, e.probes);
    e.haveProbes = true;
}

/// Store a report as `reported`, stamped with the rev the node now runs.
static void keepReport(Entry& e, JsonDocument& doc, JsonObjectConst rep, NodeConfig& scratch) {
    JsonObject r = doc["reported"].to<JsonObject>();
    r.set(rep);
    r["rev"]   = e.s.applied;
    r["local"] = false;
    takeProbes(e, r, scratch);
}

/// A node that has applied `desired` does not report it again (§5; a WiFi
/// node only reports after a boot or a local edit), so what it runs is that
/// document — kept as `reported`, with the secrets blanked like a GET's.
static void reportedFromDesired(Entry& e, Work* w) {
    w->aux.set(w->doc["desired"]);
    ncr::secretsForGet(w->aux.as<JsonObject>(), w->doc["sec_known"] | 0);
    keepReport(e, w->doc, w->aux.as<JsonObjectConst>(), w->cfg);
}

#ifdef FEATURE_ESPNOW_INGEST
static void tableFollows(const Entry& e, const NodeConfig& c) {
    if (e.espnow) espnowUpdateNode(e.id, ncr::validName(c.name) ? c.name : nullptr, c.interval_s);
}
#else
static void tableFollows(const Entry&, const NodeConfig&) {}
#endif

/// Adopt a report as the desired config (§3, §5), into w->cfg: decoded over
/// what was desired (or the defaults, for a node seen for the first time,
/// whose entry is allocated here), at `rev`, the node running `applied`. The
/// caller finishes with adoptDone(). False when the report does not decode.
///
/// During a handover the next network is kept over what the node reported
/// (ncr::hoKeepNext); when the node lacks it, it is sent in one more rev,
/// which is then the one it must run to be ready, and w->hoKept is set.
static bool adopt(Entry*& e, Work* w, JsonObjectConst rep, bool espnow, const char* name,
                  uint8_t id, uint16_t rev, uint16_t applied) {
    const bool had = e != nullptr;
    if (!had) w->doc.clear();
    decodeDesired(w->doc, w->cfg, espnow);
    w->old    = w->cfg;
    w->hoKept = false;
    Issue is;
    if (!decodeConfig(rep, w->cfg, NCJ_DEC_REV | NCJ_DEC_IDENTITY, &is)) {
        Serial.printf("[nodecfg] report from %s/%u refused: %s %s\n", name, id, is.field,
                      is.reason);
        return false;
    }
    if (!e && (e = alloc(espnow, name, id)) == nullptr) return false;
    w->cfg.transport = espnow ? Transport::EspNow : Transport::Wifi;
    w->cfg.rev   = rev;
    w->cfg.local = false;
    e->s.rev     = rev;
    e->s.applied = applied;
    e->s.status  = ncr::statusAfterApplied(ncr::ST_PENDING, rev, applied);
    e->s.err.field[0] = e->s.err.reason[0] = '\0';
    noteRev(*e, applied);
    if (had && s_ho.active && ncr::hoKeepNext(w->cfg, w->old, s_ho.ssid)) {
        bumped(*e, w->cfg);
        e->s.hoRev = e->s.rev;
        w->hoKept  = true;
    }
    return true;
}

static void adoptDone(Entry& e, Work* w) {
    copyStr(e.dname, sizeof(e.dname), w->cfg.name);
    encodeDesired(w->doc, w->cfg);
}

// ============================================================================
// Boot
// ============================================================================

void nodeCfgBegin() {
    if (s_mx) return;
    Work* w = new (std::nothrow) Work;
    if (!w) return;

    // Names first, under one hold of fsMutex; the files are read afterwards,
    // each under its own, so StorageTask is not held off for the whole scan.
    char names[NODECFG_MAX_NODES + 2][ncr::FILE_NAME_CAP];
    int  n = 0;
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        if (!fsMutex || g.isLocked()) {
            if (!LittleFS.exists(NODES_DIR)) LittleFS.mkdir(NODES_DIR);
            File d = LittleFS.open(NODES_DIR);
            if (d && d.isDirectory()) {
                while (File f = d.openNextFile()) {
                    // A name that does not fit is no node's; cut, it could be.
                    if (n < (int)(sizeof(names) / sizeof(names[0])) &&
                        strlen(f.name()) < sizeof(names[0]))
                        copyStr(names[n++], sizeof(names[0]), f.name());
                    f.close();
                }
            }
        }
    }

    for (int i = 0; i < n; i++) {
        // "w_balcony.json" is node "w:balcony"; the rest (handover.json, a
        // stray *.tmp) is refused, and so is a second file for one node.
        bool espnow; char name[NODE_NAME_MAX + 1]; uint8_t id;
        if (!ncr::keyFromFileName(names[i], espnow, name, id)) continue;
        if (find(espnow, name, id)) continue;
        Entry* e = alloc(espnow, name, id);
        if (!e || !loadEntry(*e, w->doc)) { if (e) e->used = false; continue; }

        JsonObjectConst d   = w->doc.as<JsonObjectConst>();
        JsonObjectConst des = d["desired"];
        JsonObjectConst er  = d["error"];
        e->s.rev     = des["rev"] | 0;
        e->s.applied = d["applied_rev"] | 0;
        e->s.hoRev   = d["ho_rev"] | 0;
        e->told      = d["told_rev"] | 0;
        e->s.status  = ncr::statusParse(d["status"] | "");
        if (e->s.status == ncr::ST_NONE)
            e->s.status = ncr::statusAfterApplied(ncr::ST_PENDING, e->s.rev, e->s.applied);
        copyStr(e->s.err.field, sizeof(e->s.err.field), er["field"] | "");
        copyStr(e->s.err.reason, sizeof(e->s.err.reason), er["reason"] | "");
        copyStr(e->dname, sizeof(e->dname), des["name"] | "");
        takeProbes(*e, d["reported"], w->cfg);
    }

    if (readJson(HO_PATH, w->doc) && (w->doc["ssid"] | "")[0]) {
        s_ho.active  = true;
        s_ho.startMs = millis();
        copyStr(s_ho.ssid, sizeof(s_ho.ssid), w->doc["ssid"] | "");
    }
    delete w;

    s_mx = xSemaphoreCreateMutex();
}

uint32_t nodeCfgGeneration() { return s_gen; }

void nodeCfgPutSummary(JsonObject node, bool espnow, const char* name, uint8_t id) {
    NodeCfgSummary s;
    {
        NC_LOCK(500, );
        const Entry* e = find(espnow, name, id);
        if (!e) return;
        s = e->s;
    }
    char key[ncr::KEY_CAP];
    ncr::formatKey(key, espnow, name, id);
    JsonObject c = node["cfg"].to<JsonObject>();
    c["key"]         = (const char*)key;
    c["rev"]         = s.rev;
    c["applied_rev"] = s.applied;
    c["status"]      = ncr::statusName(s.status);
    if (s.status == ncr::ST_REJECTED) putError(c["error"], s);   // only then (§7)
}

// ============================================================================
// WiFi node — §3
// ============================================================================

void nodeCfgIngest(const char* node, JsonObjectConst body, JsonObject reply) {
    const long cr = body["cfg_rev"] | -1L;        // absent or not a whole number: -1
    if (cr < 0 || cr > 0xFFFF || !ncr::validName(node)) return;
    const uint16_t nodeRev = (uint16_t)cr;
    JsonObjectConst rep = body["cfg"];
    JsonObjectConst ce  = body["cfg_error"];

    NC_LOCK(2000, );
    Entry* e = find(false, node, 0);
    if (!e) {
        // A node the collector renamed now posts under its new name. Its file
        // is still under the old one; move it rather than lose it.
        for (Entry& x : s_e) {
            if (!x.used || x.espnow || !ncr::renamedTo(node, x.dname, x.sname)) continue;
            char from[ncr::PATH_CAP], to[ncr::PATH_CAP];
            pathOf(x, from);
            ncr::filePath(to, false, node, 0);
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (fsMutex && !g.isLocked()) break;
            if (LittleFS.rename(from, to)) { copyStr(x.name, sizeof(x.name), node); e = &x; }
            break;
        }
    }
    if (!e && rep.isNull()) return;       // nothing held, nothing reported

    Work* w = new (std::nothrow) Work;
    if (!w) return;
    bool haveFile = e && loadEntry(*e, w->doc);
    if (e && !haveFile) {                 // the file went; so does the entry
        e->used = false;
        e = nullptr;
        if (rep.isNull()) { delete w; return; }
    }

    bool     save    = false;
    bool     adopted = false;
    uint16_t refused = 0;
    // A rev past the desired one (a restored node, a report refused below)
    // is one the next edit must not reuse.
    if (e && noteRev(*e, nodeRev)) save = true;

    if (!ce.isNull()) {
        refused = ce["rev"] | 0;
        if (e && refused == e->s.rev && e->s.status != ncr::ST_REJECTED) {
            e->s.status = ncr::ST_REJECTED;
            copyStr(e->s.err.field, sizeof(e->s.err.field), ce["field"] | "");
            copyStr(e->s.err.reason, sizeof(e->s.err.reason), ce["reason"] | "");
            save = true;
        }
    }

    uint16_t applied = nodeRev;
    if (!rep.isNull()) {
        const bool     local = rep["local"] | false;
        const uint16_t rrev  = rep["rev"] | nodeRev;
        const ncr::ReportPlan plan = ncr::planReport(e != nullptr, e ? e->s.rev : 0, rrev, local);
        applied = plan.applied;
        if (!plan.adopt) {
            e->s.applied = applied;     // `have` was true, so e is set
            e->s.status  = ncr::statusAfterApplied(e->s.status, e->s.rev, applied);
            if (ncr::revAtOrPast(applied, e->s.rev)) w->doc["sec_dirty"] = 0;
        } else if (adopt(e, w, rep, false, node, 0, plan.rev, plan.rev)) {
            adoptDone(*e, w);
            // The node's own secrets are what it runs; the collector's copy
            // may be stale, so none is sent until it is edited here again.
            uint8_t known = ncr::reportSecretsKnown(rep), dirty = 0;
            // ...except the handover's next passphrase, when it was put back.
            if (w->hoKept) ncr::secretsEdited(ncr::SEC_NPASS, w->cfg, known, dirty);
            w->doc["sec_known"] = known;
            w->doc["sec_dirty"] = dirty;
            adopted = true;
        } else if (!e) {
            delete w;
            return;
        } else {
            // Refused (logged by adopt()): like the ESP-NOW path, nothing of
            // it is kept — not as `reported`, and not the rev it would have
            // been adopted at as the one the node runs.
            applied = e->s.applied;
        }
        if (e && adopted == plan.adopt) { keepReport(*e, w->doc, rep, w->cfg); save = true; }
    }

    if (e && !adopted && applied != e->s.applied) {
        e->s.applied = applied;
        e->s.status  = ncr::statusAfterApplied(e->s.status, e->s.rev, applied);
        if (ncr::revAtOrPast(applied, e->s.rev)) {
            w->doc["sec_dirty"] = 0;
            if (rep.isNull()) reportedFromDesired(*e, w);
        }
        save = true;
    }

    if (e) {
        e->seenMs = millis() | 1;
        switch (ncr::ingestReply(adopted, e->s.status, e->s.rev, applied, refused)) {
            case ncr::REPLY_REV:
                // The rev its edit was adopted at — which is behind the
                // desired one when a handover's next network was put back
                // (adopt()), so the node is sent that on its next POST.
                reply["cfg"]["rev"] = applied;
                break;
            case ncr::REPLY_FULL: {
                JsonObject c = reply["cfg"].to<JsonObject>();
                c.set(w->doc["desired"].as<JsonObjectConst>());
                ncr::secretsForNode(c, w->doc["sec_dirty"] | 0);
                copyStr(e->sname, sizeof(e->sname), c["name"] | "");
                break;
            }
            default:
                break;
        }
        if (save) saveEntry(*e, w->doc);
    }
    delete w;
}

// ============================================================================
// ESP-NOW node — §5
// ============================================================================

bool nodeCfgRadioState(uint8_t id, NodeCfgRadio& r) {
    r = NodeCfgRadio{ false, 0, 0, ncr::ST_NONE };
    NC_LOCK(200, false);
    const Entry* e = find(true, nullptr, id);
    if (e) { r.have = true; r.rev = e->s.rev; r.applied = e->s.applied; r.status = e->s.status; }
    return true;
}

void nodeCfgEspnowReport(uint8_t id, const char* json, size_t len,
                         const char* label, uint16_t intervalS, ncr::ReportPlan plan) {
    Work* w = new (std::nothrow) Work;
    const bool parsed = w && !deserializeJson(w->aux, json, len) && w->aux.is<JsonObject>();
    do {
        MutexGuard g(s_mx, pdMS_TO_TICKS(2000));
        if (!g.isLocked()) break;
        Entry* e = find(true, nullptr, id);
        // The callback has already told the node plan.applied (at most one
        // past what it said), whether or not the report is kept below: a
        // later edit must move past it (ncr::revAfter).
        const bool moved = e && noteRev(*e, plan.applied);
        if (!parsed) {
            if (moved && w && loadEntry(*e, w->doc)) saveEntry(*e, w->doc);
            break;
        }
        JsonObjectConst rep = w->aux.as<JsonObjectConst>();
        if (e && !loadEntry(*e, w->doc)) { e->used = false; e = nullptr; }

        const bool     local = rep["local"] | false;
        const uint16_t rrev  = rep["rev"] | 0;
        // The callback's mirror said a config was held, the store has none
        // (forgotten in between): plan it as the new node it now is.
        if (!e && !plan.adopt) plan = ncr::planReport(false, 0, rrev, local);
        // A report that is not local is answered with its own rev, so the
        // node keeps it; one adopted at a new rev is then pending and fetched.
        if (!local) plan.applied = rrev;
        if (plan.adopt) {
            const bool first = (e == nullptr);
            // The node was told plan.applied; the desired rev must still move
            // past anything a web edit made since the callback's mirror.
            const uint16_t rev = (!first && ncr::revAtOrPast(e->s.rev, plan.rev))
                                     ? ncr::nextRev(e->s.rev)
                                     : plan.rev;
            if (!adopt(e, w, rep, true, "", id, rev, plan.applied)) {
                if (e && moved) saveEntry(*e, w->doc);   // keeps told_rev
                break;
            }
            // First contact: the table's label and interval win, and the node
            // is sent them in the next rev. After a local edit the table
            // follows the node instead (§0.4, local edits win).
            if (first) {
                if (ncr::adoptTableIdentity(w->cfg, label, intervalS)) bumped(*e, w->cfg);
            } else {
                tableFollows(*e, w->cfg);
            }
            adoptDone(*e, w);
        } else {
            e->s.applied = plan.applied;
            e->s.status  = ncr::statusAfterApplied(e->s.status, e->s.rev, plan.applied);
        }
        keepReport(*e, w->doc, rep, w->cfg);
        saveEntry(*e, w->doc);
    } while (false);
    delete w;
}

void nodeCfgEspnowAck(uint8_t id, uint16_t rev, bool ok, const char* field, const char* reason) {
    NC_LOCK(2000, );
    Entry* e = find(true, nullptr, id);
    if (!e) return;
    if (ok) {
        // An ACK past the desired rev (a report the store refused, or
        // missed) is a rev the next edit must not reuse.
        const bool moved = noteRev(*e, rev);
        if (rev == e->s.applied && !moved) return;
        e->s.applied = rev;
        e->s.status  = ncr::statusAfterApplied(e->s.status, e->s.rev, rev);
    } else {
        if (rev != e->s.rev || e->s.status == ncr::ST_REJECTED) return;
        e->s.status = ncr::ST_REJECTED;
        copyStr(e->s.err.field, sizeof(e->s.err.field), field);
        copyStr(e->s.err.reason, sizeof(e->s.err.reason), reason);
    }
    Work* w = new (std::nothrow) Work;
    if (!w) { s_gen++; return; }
    if (loadEntry(*e, w->doc)) {
        if (ok && rev == e->s.rev) reportedFromDesired(*e, w);
        saveEntry(*e, w->doc);
    } else {
        s_gen++;
    }
    delete w;
}

size_t nodeCfgRadioDoc(uint8_t id, char* buf, size_t cap, uint16_t& rev) {
    NC_LOCK(2000, 0);
    const Entry* e = find(true, nullptr, id);
    if (!e || !ncr::shouldSend(e->s.status, e->s.rev, e->s.applied)) return 0;
    Work* w = new (std::nothrow) Work;
    if (!w) return 0;
    size_t n = 0;
    if (loadEntry(*e, w->doc)) {
        // `desired` of an ESP-NOW node was written from a config whose
        // transport is espnow, so it has no `net` and no secrets to strip.
        JsonVariantConst d = w->doc["desired"];
        const size_t need = measureJson(d);
        if (need <= EN_CFG_MAX_TOTAL && need < cap) {
            n = serializeJson(d, buf, cap);
            rev = e->s.rev;
        } else {
            Serial.printf("[nodecfg] config for node %u is %u bytes — too big for the radio\n",
                          id, (unsigned)need);
        }
    }
    delete w;
    return n;
}

int nodeCfgData2Named(uint8_t id, const Data2Sample& s, ncr::NamedValue* out, int max) {
    NodeConfig c;
    bool have = false;
    {
        MutexGuard g(s_mx, pdMS_TO_TICKS(200));
        const Entry* e = g.isLocked() ? find(true, nullptr, id) : nullptr;
        if (e && e->haveProbes) { ncr::probeMapToConfig(e->probes, c); have = true; }
    }
    float battV = 0.0f;
    return ncr::data2Named(have ? &c : nullptr, s, out, max, battV);
}

void nodeCfgForget(bool espnow, const char* name, uint8_t id) {
    NC_LOCK(2000, );
    Entry* e = find(espnow, name, id);
    if (!e) return;
    char p[ncr::PATH_CAP];
    pathOf(*e, p);
    removeFile(p);
    e->used = false;
    s_gen++;
}

// ============================================================================
// The Nodes page — §7
// ============================================================================

static int fail400(JsonDocument& out, const char* field, const char* reason) {
    out["ok"]     = false;
    out["field"]  = field;
    out["reason"] = reason;
    return 400;
}

int nodeCfgApiGet(const char* key, JsonDocument& out) {
    bool espnow; char name[NODE_NAME_MAX + 1]; uint8_t id;
    if (!ncr::parseKey(key, espnow, name, id)) return fail400(out, "key", "not a node key");

    Work* w = new (std::nothrow) Work;
    if (!w) return 500;
    int code = 200;
    {
        MutexGuard g(s_mx, pdMS_TO_TICKS(2000));
        const Entry* e = g.isLocked() ? find(espnow, name, id) : nullptr;
        if (!g.isLocked()) {
            code = 503;
        } else {
            out["key"]       = key;
            out["transport"] = espnow ? "espnow" : "wifi";
            Hw hw = espnow ? Hw::Esp32c3 : Hw::Esp8266;
            if (e && loadEntry(*e, w->doc)) {
                JsonObject d = out["desired"].to<JsonObject>();
                d.set(w->doc["desired"].as<JsonObjectConst>());
                ncr::secretsForGet(d, w->doc["sec_known"] | 0);
                out["reported"].set(w->doc["reported"]);
                out["applied_rev"] = e->s.applied;
                out["status"]      = ncr::statusName(e->s.status);
                putError(out["error"], e->s);
                parseHw(w->doc["reported"]["hw"] | "", hw);
            } else {
                out["desired"]     = nullptr;
                out["reported"]    = nullptr;
                out["applied_rev"] = 0;
                out["status"]      = nullptr;
                out["error"]       = nullptr;
            }
            encodeCaps(espnow ? Transport::EspNow : Transport::Wifi, hw,
                       out["caps"].to<JsonObject>());
        }
    }
    delete w;
    return code;
}

int nodeCfgApiPost(JsonObjectConst body, JsonDocument& out) {
    bool espnow; char name[NODE_NAME_MAX + 1]; uint8_t id;
    if (!ncr::parseKey(body["key"] | "", espnow, name, id))
        return fail400(out, "key", "not a node key");
    JsonVariantConst in = body["config"];
    if (!in.is<JsonObjectConst>()) return fail400(out, "config", "must be an object");

    Work* w = new (std::nothrow) Work;
    if (!w) return 500;
    int code = 200;
    do {
        MutexGuard g(s_mx, pdMS_TO_TICKS(3000));
        if (!g.isLocked()) { code = 503; break; }
        Entry* e = find(espnow, name, id);
        if (!e || !loadEntry(*e, w->doc) || !decodeDesired(w->doc, w->cfg, espnow)) {
            code = fail400(out, "key", "no config for this node yet");
            break;
        }
        w->old = w->cfg;
        Issue is;
        if (!decodeConfig(in, w->cfg, 0, &is)) { code = fail400(out, is.field, is.reason); break; }
        // The next network is the handover's to set and to take back (§4),
        // not the page's: an edit made during a handover must not drop it.
        w->cfg.net.next = w->old.net.next;
        copyStr(w->cfg.link.next_ssid, sizeof(w->cfg.link.next_ssid), w->old.link.next_ssid);

        if (!validate(w->cfg, w->v)) {
            encodeValidation(w->v, out.to<JsonObject>());
            code = 400;
            break;
        }
        uint8_t known = w->doc["sec_known"] | 0, dirty = w->doc["sec_dirty"] | 0;
        ncr::secretsEdited(ncr::secretsDiff(w->old, w->cfg), w->cfg, known, dirty);
        w->doc["sec_known"] = known;
        w->doc["sec_dirty"] = dirty;
        bumped(*e, w->cfg);
        encodeDesired(w->doc, w->cfg);
        if (!saveEntry(*e, w->doc)) { code = 500; out["ok"] = false; out["reason"] = "could not save"; break; }
        tableFollows(*e, w->cfg);
        encodeValidation(w->v, out.to<JsonObject>());
        out["rev"] = e->s.rev;
    } while (false);
    delete w;
    return code;
}

// ============================================================================
// Handover — §4
// ============================================================================

bool nodeCfgHandover(char ssid[SSID_CAP], uint32_t* startMs) {
    NC_LOCK(500, false);
    if (ssid)    copyStr(ssid, SSID_CAP, s_ho.ssid);
    if (startMs) *startMs = s_ho.startMs;
    return s_ho.active;
}

int nodeCfgHandoverSort(JsonObject out) {
    struct K {
        bool    espnow, offline;
        uint8_t id;
        char    name[NODE_NAME_MAX + 1];
    };
    K   k[NODECFG_LIST_MAX];
    int n = 0;
    NC_LOCK(1000, 0);
    // The slot for a node, added (offline until a list says otherwise) when
    // it is not there yet; null when full.
    auto slot = [&](bool espnow, const char* name, uint8_t id) -> K* {
        for (int i = 0; i < n; i++)
            if (k[i].espnow == espnow && (espnow ? k[i].id == id : !strcmp(k[i].name, name)))
                return &k[i];
        if (n >= NODECFG_LIST_MAX) return nullptr;
        K& x = k[n++];
        x.espnow = espnow; x.offline = true; x.id = id;
        copyStr(x.name, sizeof(x.name), name);
        return &x;
    };
    // A WiFi node is alive when it posted lately. Its own POSTs say so
    // (seenMs), since one that only ever posts "readings": [] — or whose
    // readings found the mailbox full — is not in remoteIngest at all.
    for (const Entry& e : s_e)
        if (e.used)
            if (K* x = slot(e.espnow, e.name, e.id))
                x->offline = !e.seenMs || millis() - e.seenMs >= REMOTE_STATUS_STALE_MS;
#ifdef FEATURE_ESPNOW_INGEST
    {
        EspNowNode nodes[ESPNOW_MAX_NODES];
        const int      c   = espnowCopyNodes(nodes, ESPNOW_MAX_NODES);
        const uint32_t now = millis();
        const uint8_t  iv  = espnowGetOfflineIntervals();
        for (int j = 0; j < c; j++)
            if (K* x = slot(true, "", nodes[j].nodeId)) x->offline = espnowNodeOffline(nodes[j], now, iv);
    }
#endif
    char nid[RemoteIngest::MAX_NODE_ID];
    for (int j = 0; remoteIngest.nodeIdAt(j, nid, sizeof(nid)); j++) {
        const uint32_t age = remoteIngest.ageMsForNode(nid);
        if (K* x = slot(false, nid, 0))
            if (age < REMOTE_STATUS_STALE_MS) x->offline = false;
    }

    JsonArray lists[3];
    if (!out.isNull()) {
        lists[ncr::HO_READY]   = out["ready"].to<JsonArray>();
        lists[ncr::HO_PENDING] = out["pending"].to<JsonArray>();
        lists[ncr::HO_OFFLINE] = out["offline"].to<JsonArray>();
    }
    int pending = 0;
    for (int i = 0; i < n; i++) {
        const Entry*  e = find(k[i].espnow, k[i].name, k[i].id);
        const uint8_t c = ncr::hoClassify(e != nullptr, e ? e->s.applied : 0, e ? e->s.hoRev : 0,
                                          k[i].offline);
        if (c == ncr::HO_PENDING) pending++;
        if (out.isNull()) continue;
        char key[ncr::KEY_CAP];
        ncr::formatKey(key, k[i].espnow, k[i].name, k[i].id);
        lists[c].add((const char*)key);
    }
    return pending;
}

/// Hand every node the next network (ssid "" takes it back) in a new rev.
static void handAll(Work* w, const char* ssid, const char* pass, bool start) {
    for (Entry& e : s_e) {
        if (!e.used || !loadEntry(e, w->doc) || !decodeDesired(w->doc, w->cfg, e.espnow)) continue;
        const uint8_t changed = ncr::hoApplyNext(w->cfg, ssid, pass);
        uint8_t known = w->doc["sec_known"] | 0, dirty = w->doc["sec_dirty"] | 0;
        ncr::secretsEdited(changed, w->cfg, known, dirty);
        w->doc["sec_known"] = known;
        w->doc["sec_dirty"] = dirty;
        bumped(e, w->cfg);
        e.s.hoRev = start ? e.s.rev : 0;
        encodeDesired(w->doc, w->cfg);
        saveEntry(e, w->doc);
    }
}

/// Start (ssid set) or cancel (ssid "") — one path, as the two differ only
/// in what is handed out and whether the file is written or removed.
bool nodeCfgHandoverStart(const char* ssid, const char* pass, JsonVariantConst form) {
    Work* w = new (std::nothrow) Work;
    if (!w) return false;
    bool ok = false;
    {
        MutexGuard g(s_mx, pdMS_TO_TICKS(3000));
        if (g.isLocked()) {
            const bool start = ssid[0] != '\0';
            if (start) {
                w->aux["ssid"] = ssid;
                w->aux["pass"] = pass;
                if (!form.isNull()) w->aux["form"].set(form);
                ok = writeJson(HO_PATH, w->aux);
            } else {
                removeFile(HO_PATH);
                ok = true;
            }
            if (ok) {
                if (start || s_ho.active) handAll(w, ssid, pass, start);
                s_ho.active  = start;
                s_ho.startMs = millis();
                copyStr(s_ho.ssid, sizeof(s_ho.ssid), ssid);
            }
        }
    }
    delete w;
    return ok;
}

bool nodeCfgHandoverCancel() { return nodeCfgHandoverStart("", "", JsonVariantConst()); }

bool nodeCfgHandoverFinish(JsonDocument& out) {
    NC_LOCK(3000, false);
    if (!s_ho.active || !readJson(HO_PATH, out)) return false;
    s_ho.active = false;
    removeFile(HO_PATH);
    for (Entry& e : s_e) e.s.hoRev = 0;
    s_gen++;
    return true;
}

#endif  // FEATURE_REMOTE_NODES
