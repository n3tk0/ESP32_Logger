#include "NodeFwStore.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <new>
#include <time.h>

#include "NodeCfgRules.h"             // ncr::parseKey / formatKey / KEY_CAP
#include "NodeCfgStore.h"             // nodeCfgHandoverSort(): every node the collector knows
#include "../core/Globals.h"          // sdAvailable
#include "../core/SdCompat.h"         // sdFs()
#include "../pipeline/DataPipeline.h" // fsMutex
#include "../utils/AtomicWrite.h"
#include "../utils/MutexGuard.h"

using nodefw::Kind;

// ============================================================================
// State
// ============================================================================

struct Img {
    bool     have;
    uint8_t  attempt;     ///< kept without an image too: it is the kind's counter
    uint16_t minMv;       ///< espnow-c3; survives a new upload
    uint32_t serial;      ///< moves on every replace / delete (nodeFwRead)
    uint32_t size;
    uint32_t imgId;
    uint32_t uploaded;
    char     ver[nodefw::VER_CAP];
    char     md5[33];
    char     sha[65];
};

struct Target {
    char    key[ncr::KEY_CAP];   ///< "" = free slot; the kind is the key's
    uint8_t st;
    uint8_t pct;                 ///< RAM only (§2.2)
    char    err[40];
};

static Img               s_img[3];   // by nodefw::Kind; [0] unused
static Target            s_t[NODEFW_MAX_TARGETS];
static SemaphoreHandle_t s_mx  = nullptr;
static volatile uint32_t s_gen = 1;

static const uint16_t DEFAULT_MIN_MV = 3600;
static const char     ROLLOUT[]      = NODEFW_DIR "/rollout.json";

#define NF_LOCK(ms, fail)                                   \
    MutexGuard _g(s_mx, pdMS_TO_TICKS(ms));                 \
    if (!_g.isLocked()) return fail

static bool kindOk(Kind k) { return k == nodefw::KIND_ESP8266 || k == nodefw::KIND_ESPNOW_C3; }

static void filePath(char* out, size_t cap, Kind k, const char* ext) {
    snprintf(out, cap, NODEFW_DIR "/%s.%s", nodefw::kindName(k), ext);
}

static Target* findT(const char* key) {
    for (Target& t : s_t)
        if (t.key[0] && strcmp(t.key, key) == 0) return &t;
    return nullptr;
}

static void setSt(Target& t, uint8_t st, const char* err) {
    t.st = st;
    t.pct = 0;
    snprintf(t.err, sizeof(t.err), "%s", err ? err : "");
}

// ============================================================================
// Files — every access under fsMutex (Pillar 1.3), always after s_mx
// ============================================================================

static bool readJson(const char* path, JsonDocument& doc) {
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return false;
    File f = sdFs()->open(path, "r");
    if (!f) return false;
    const bool ok = !deserializeJson(doc, f);
    f.close();
    return ok;
}

static bool writeJson(const char* path, const JsonDocument& doc) {
    return atomicWrite(*sdFs(), path,
                       [&doc](File& f) { return serializeJson(doc, static_cast<Print&>(f)) > 0; }, fsMutex);
}

static void removeFile(const char* path) {
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return;
    sdFs()->remove(path);
}

/// The image's own JSON (§2.1), from RAM.
static void putMeta(JsonObject o, Kind k) {
    const Img& m = s_img[k];
    o["kind"]     = nodefw::kindName(k);
    o["ver"]      = (const char*)m.ver;
    o["size"]     = m.size;
    o["md5"]      = (const char*)m.md5;
    o["sha256"]   = (const char*)m.sha;
    o["uploaded"] = m.uploaded;
    if (k == nodefw::KIND_ESPNOW_C3) {
        o["img_id"] = m.imgId;
        o["min_mv"] = m.minMv;
    }
}

static bool saveMeta(Kind k) {
    char p[40];
    filePath(p, sizeof(p), k, "json");
    JsonDocument d;
    putMeta(d.to<JsonObject>(), k);
    return writeJson(p, d);
}

/// Status changed: the generation moves, and rollout.json is rewritten from
/// RAM (RAM is what everything reads, so a failed write costs only the
/// status surviving a reboot).
static void saveRollout() {
    s_gen++;
    if (!sdAvailable) return;
    JsonDocument d;
    for (int k = nodefw::KIND_ESP8266; k <= nodefw::KIND_ESPNOW_C3; k++) {
        JsonObject o = d[nodefw::kindName((Kind)k)].to<JsonObject>();
        o["attempt"] = s_img[k].attempt;
        JsonObject ts = o["targets"].to<JsonObject>();
        for (const Target& t : s_t) {
            if (!t.key[0] || nfr::kindOfKey(t.key) != (Kind)k) continue;
            JsonObject x = ts[(const char*)t.key].to<JsonObject>();
            x["st"] = nfr::statusName(t.st);
            if (t.err[0]) x["err"] = (const char*)t.err;
        }
    }
    if (!writeJson(ROLLOUT, d)) Serial.println("[nodefw] could not write rollout.json");
}

// ============================================================================
// Boot
// ============================================================================

void nodeFwBegin() {
    if (s_mx) return;
    for (Img& m : s_img) { m.minMv = DEFAULT_MIN_MV; m.serial = 1; }
    if (sdAvailable) {
        JsonDocument d;
        for (int k = nodefw::KIND_ESP8266; k <= nodefw::KIND_ESPNOW_C3; k++) {
            Img& m = s_img[k];
            char p[40];
            filePath(p, sizeof(p), (Kind)k, "json");
            if (!readJson(p, d)) continue;
            m.size     = d["size"] | 0UL;
            m.imgId    = d["img_id"] | 0UL;
            m.uploaded = d["uploaded"] | 0UL;
            m.minMv    = d["min_mv"] | DEFAULT_MIN_MV;
            snprintf(m.ver, sizeof(m.ver), "%s", d["ver"] | "");
            snprintf(m.md5, sizeof(m.md5), "%s", d["md5"] | "");
            snprintf(m.sha, sizeof(m.sha), "%s", d["sha256"] | "");
            filePath(p, sizeof(p), (Kind)k, "bin");
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (fsMutex && !g.isLocked()) continue;
            File f = sdFs()->open(p, "r");
            // Only an image whose file is there, at the size it was checked at.
            m.have = f && f.size() == m.size && m.size > 0;
            if (f) f.close();
        }
        if (readJson(ROLLOUT, d)) {
            int n = 0;
            for (int k = nodefw::KIND_ESP8266; k <= nodefw::KIND_ESPNOW_C3; k++) {
                JsonObjectConst o = d[nodefw::kindName((Kind)k)];
                s_img[k].attempt = o["attempt"] | 0;
                // A rollout without its image is dropped with it (§2.1).
                if (!s_img[k].have) continue;
                for (JsonPairConst kv : o["targets"].as<JsonObjectConst>()) {
                    const uint8_t st = nfr::statusParse(kv.value()["st"] | "");
                    if (n >= NODEFW_MAX_TARGETS || st == nfr::ST_NONE ||
                        nfr::kindOfKey(kv.key().c_str()) != (Kind)k ||
                        strlen(kv.key().c_str()) >= ncr::KEY_CAP)
                        continue;
                    Target& t = s_t[n++];
                    snprintf(t.key, sizeof(t.key), "%s", kv.key().c_str());
                    // Progress is RAM only: a download cut by a reboot is
                    // pending again until the node asks.
                    setSt(t, st == nfr::ST_SENDING ? nfr::ST_PENDING : st, kv.value()["err"] | "");
                }
            }
        }
    }
    s_mx = xSemaphoreCreateMutex();
}

uint32_t nodeFwGeneration() { return s_gen; }

// ============================================================================
// Image bytes
// ============================================================================

bool nodeFwImage(Kind k, NodeFwImage& o) {
    memset(&o, 0, sizeof(o));
    NF_LOCK(1000, false);
    if (!kindOk(k) || !s_img[k].have) return true;
    const Img& m = s_img[k];
    o.serial  = m.serial;
    o.size    = m.size;
    o.imgId   = m.imgId;
    o.minMv   = m.minMv;
    o.attempt = m.attempt;
    memcpy(o.md5, m.md5, sizeof(o.md5));
    return true;
}

int nodeFwRead(Kind k, uint32_t serial, uint32_t off, uint8_t* buf, size_t len) {
    if (!kindOk(k) || !sdAvailable) return 0;
    // fsMutex only, not s_mx: a replace or delete moves the serial while it
    // holds fsMutex (nodeFwCommit, the delete action), so checking it under
    // fsMutex is enough — and the download's many small reads never queue
    // behind a store operation that is waiting for the card.
    MutexGuard g(fsMutex, pdMS_TO_TICKS(1000));
    if (fsMutex && !g.isLocked()) return -1;
    const Img& m = s_img[k];
    if (!m.have || m.serial != serial || off >= m.size) return 0;
    if (len > m.size - off) len = m.size - off;
    char p[40];
    filePath(p, sizeof(p), k, "bin");
    File f = sdFs()->open(p, "r");
    int n = 0;
    if (f && f.seek(off)) n = (int)f.read(buf, len);
    if (f) f.close();
    return n > 0 ? n : 0;
}

bool nodeFwCommit(const NodeFwMeta& u) {
    if (!kindOk(u.kind)) return false;
    NF_LOCK(3000, false);
    Img& m = s_img[u.kind];
    char p[40];
    filePath(p, sizeof(p), u.kind, "bin");
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
        if (fsMutex && !g.isLocked()) return false;
        // Gone before the rename: a download of the old image must not read
        // the new one's bytes (nodeFwRead checks the serial under fsMutex).
        m.have = false;
        m.serial++;
        sdFs()->remove(p);                        // FAT will not rename over it
        if (!sdFs()->rename(NODEFW_TMP, p)) return false;
        m.have     = true;
        m.size     = u.size;
        m.imgId    = u.imgId;
        const uint32_t now = (uint32_t)time(nullptr);
        m.uploaded = now >= 1000000000u ? now : 0;
        memcpy(m.ver, u.ver, sizeof(m.ver));
        memcpy(m.md5, u.md5, sizeof(m.md5));
        memcpy(m.sha, u.sha256, sizeof(m.sha));
    }
    saveMeta(u.kind);
    // §2.1: the targets stay chosen, every one pending again, on a new attempt.
    m.attempt = nfr::nextAttempt(m.attempt);
    for (Target& t : s_t)
        if (t.key[0] && nfr::kindOfKey(t.key) == u.kind) setSt(t, nfr::ST_PENDING, nullptr);
    saveRollout();
    return true;
}

// ============================================================================
// WiFi node — §3
// ============================================================================

void nodeFwIngest(const char* node, JsonObjectConst body, JsonObject reply) {
    if (!ncr::validName(node)) return;
    char key[ncr::KEY_CAP];
    ncr::formatKey(key, false, node, 0);
    JsonObjectConst er = body["fw_error"];

    NF_LOCK(2000, );
    Target* t = findT(key);
    const Img& m = s_img[nodefw::KIND_ESP8266];
    if (!t || !m.have) return;
    const uint8_t act = nfr::wifiIngest(t->st, body["fw_md5"] | "", m.md5,
                                        er.isNull() ? nullptr : (er["md5"] | ""),
                                        er["attempt"] | -1L, m.attempt);
    switch (act) {
        case nfr::W_DONE:
            setSt(*t, nfr::ST_DONE, nullptr);
            saveRollout();
            break;
        case nfr::W_FAILED:
            setSt(*t, nfr::ST_FAILED, er["reason"] | "failed");
            saveRollout();
            break;
        case nfr::W_OFFER: {
            JsonObject f = reply["fw"].to<JsonObject>();
            char path[48];
            snprintf(path, sizeof(path), "/api/nodes/fw/bin?kind=%s", nodefw::kindName(nodefw::KIND_ESP8266));
            f["path"]    = (const char*)path;
            f["md5"]     = (const char*)m.md5;
            f["size"]    = m.size;
            f["ver"]     = (const char*)m.ver;
            f["attempt"] = m.attempt;
            if (t->st != nfr::ST_SENDING) {
                setSt(*t, nfr::ST_SENDING, nullptr);
                saveRollout();
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// ESP-NOW node — §4
// ============================================================================

static Target* espnowTarget(uint8_t id) {
    char key[ncr::KEY_CAP];
    ncr::formatKey(key, true, "", id);
    return findT(key);
}

uint8_t nodeFwRadioStatus(uint8_t id) {
    NF_LOCK(200, NODEFW_ST_BUSY);
    const Target* t = espnowTarget(id);
    return t && s_img[nodefw::KIND_ESPNOW_C3].have ? t->st : nfr::ST_NONE;
}

void nodeFwEspnowGet(uint8_t id, uint32_t imgId, uint32_t off) {
    NF_LOCK(500, );
    const Img& m = s_img[nodefw::KIND_ESPNOW_C3];
    Target* t = espnowTarget(id);
    if (!t || !m.have || imgId != m.imgId || !nfr::isOpen(t->st)) return;
    if (nfr::getMeansSending(t->st)) {
        setSt(*t, nfr::ST_SENDING, nullptr);
        saveRollout();
    }
    t->pct = nfr::pct(off, m.size);
}

void nodeFwEspnowDone(uint8_t id, uint32_t imgId, uint8_t status, uint8_t attempt, uint16_t value) {
    NF_LOCK(2000, );
    const Img& m = s_img[nodefw::KIND_ESPNOW_C3];
    Target* t = espnowTarget(id);
    if (!t) return;
    const uint8_t st = nfr::afterDone(t->st, m.have && imgId == m.imgId, status, attempt, m.attempt);
    if (st == nfr::ST_NONE) return;
    char err[sizeof(t->err)];
    nfr::doneError(err, sizeof(err), status, value);
    setSt(*t, st, err);
    Serial.printf("[nodefw] node %u: %s %s\n", id, nfr::statusName(st), err);
    saveRollout();
}

// ============================================================================
// The Nodes page — §2.3
// ============================================================================

void nodeFwApiGet(JsonDocument& out) {
    out["sd"] = sdAvailable;
    JsonObject im = out["images"].to<JsonObject>();
    JsonObject ts = out["targets"].to<JsonObject>();
    NF_LOCK(2000, );
    for (int k = nodefw::KIND_ESP8266; k <= nodefw::KIND_ESPNOW_C3; k++) {
        JsonVariant v = im[nodefw::kindName((Kind)k)];
        if (s_img[k].have) putMeta(v.to<JsonObject>(), (Kind)k);
        else v.set(nullptr);
    }
    for (const Target& t : s_t) {
        if (!t.key[0]) continue;
        const Kind k = nfr::kindOfKey(t.key);
        JsonObject x = ts[(const char*)t.key].to<JsonObject>();
        x["kind"] = nodefw::kindName(k);
        x["st"]   = nfr::statusName(t.st);
        if (t.st == nfr::ST_SENDING) x["pct"] = t.pct;
        if (t.err[0]) x["err"] = (const char*)t.err;
        x["attempt"] = s_img[k].attempt;
    }
}

static int fail(JsonDocument& out, int code, const char* err) {
    out["ok"]    = false;
    out["error"] = err;
    return code;
}

/// A key the action may name: a Nodes page key of this kind.
static bool keyOk(const char* key, Kind k) {
    bool espnow; char name[nodecfg::NODE_NAME_MAX + 1]; uint8_t id;
    return nfr::kindOfKey(key) == k && strlen(key) < ncr::KEY_CAP &&
           ncr::parseKey(key, espnow, name, id);
}

int nodeFwApiPost(JsonObjectConst body, JsonDocument& out) {
    const char* action = body["action"] | "";
    const Kind  k      = nodefw::kindFromName(body["kind"] | "");
    if (!kindOk(k)) return fail(out, 400, "bad_request");
    if (!sdAvailable) return fail(out, 409, "no_sd");

    const bool start = strcmp(action, "start") == 0;
    if (start || strcmp(action, "cancel") == 0) {
        // "all" to start = every node of the kind the collector knows (§2.3):
        // the handover's walk already gathers exactly that — config files,
        // both status lists — so it is asked rather than repeated here.
        JsonDocument all;
        JsonVariantConst keys = body["keys"];
        const bool every = keys.is<const char*>() && strcmp(keys.as<const char*>(), "all") == 0;
        if (every && start) {
            nodeCfgHandoverSort(all.to<JsonObject>());
        } else if (!keys.is<JsonArrayConst>()) {
            if (!every) return fail(out, 400, "bad_key");
        } else {
            for (JsonVariantConst v : keys.as<JsonArrayConst>())
                if (!keyOk(v | "", k)) return fail(out, 400, "bad_key");
        }

        NF_LOCK(3000, fail(out, 503, "busy"));
        Img& m = s_img[k];
        if (!start) {
            for (Target& t : s_t) {
                if (!t.key[0] || nfr::kindOfKey(t.key) != k) continue;
                bool drop = every;
                for (JsonVariantConst v : keys.as<JsonArrayConst>())
                    drop |= strcmp(v | "", t.key) == 0;
                if (drop) t.key[0] = '\0';
            }
            saveRollout();
            out["ok"] = true;
            return 200;
        }
        if (!m.have) return fail(out, 409, "no_image");
        int n = 0;
        auto add = [&](const char* key) {
            if (!keyOk(key, k)) return;   // "all" walks both kinds
            Target* t = findT(key);
            if (!t)
                for (Target& f : s_t)
                    if (!f.key[0]) { t = &f; snprintf(f.key, sizeof(f.key), "%s", key); break; }
            if (!t) return;               // full: counted out of `targets`
            setSt(*t, nfr::ST_PENDING, nullptr);
            n++;
        };
        if (every) {
            for (JsonPairConst l : all.as<JsonObjectConst>())
                for (JsonVariantConst v : l.value().as<JsonArrayConst>()) add(v | "");
        } else {
            for (JsonVariantConst v : keys.as<JsonArrayConst>()) add(v | "");
        }
        // §2.2 Retry: a new attempt, so a node that failed this image tries again.
        m.attempt = nfr::nextAttempt(m.attempt);
        saveRollout();
        out["ok"]      = true;
        out["targets"] = n;
        return 200;
    }

    if (strcmp(action, "delete") == 0) {
        NF_LOCK(3000, fail(out, 503, "busy"));
        Img& m = s_img[k];
        {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
            if (fsMutex && !g.isLocked()) return fail(out, 503, "busy");
            m.have = false;
            m.serial++;
            char p[40];
            filePath(p, sizeof(p), k, "bin");
            sdFs()->remove(p);
            filePath(p, sizeof(p), k, "json");
            sdFs()->remove(p);
        }
        for (Target& t : s_t)
            if (t.key[0] && nfr::kindOfKey(t.key) == k) t.key[0] = '\0';
        saveRollout();
        out["ok"] = true;
        return 200;
    }

    if (strcmp(action, "min_mv") == 0) {
        const long mv = body["min_mv"] | -1L;
        if (k != nodefw::KIND_ESPNOW_C3 || !(mv == 0 || (mv >= 3000 && mv <= 4200)))
            return fail(out, 400, "bad_request");
        NF_LOCK(3000, fail(out, 503, "busy"));
        Img& m = s_img[k];
        if (!m.have) return fail(out, 409, "no_image");
        m.minMv = (uint16_t)mv;
        s_gen++;
        if (!saveMeta(k)) return fail(out, 500, "write_failed");
        out["ok"] = true;
        return 200;
    }
    return fail(out, 400, "bad_request");
}

#endif  // FEATURE_REMOTE_NODES
