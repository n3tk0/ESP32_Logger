#include "KindlePkg.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <MD5Builder.h>
#include <new>
#include <time.h>

#include "FormArgs.h"                   // formArg / queryArg
#include "RequireAuth.h"
#include "../core/Globals.h"            // sdAvailable
#include "../core/SdCompat.h"           // sdFs()
#include "../pipeline/DataPipeline.h"   // fsMutex
#include "../utils/AtomicWrite.h"
#include "../utils/JsonIO.h"
#include "../utils/MutexGuard.h"

// ============================================================================
// State — async web task only (see the header)
// ============================================================================

struct Pkg {
    bool     have;
    bool     offer;
    uint32_t serial;      ///< moves on every replace / delete (the download)
    uint32_t size;
    uint32_t uploaded;
    char     ver[kpkg::VER_CAP];
    char     md5[33];
};

/// How a reader's update is going, as the page shows it.
enum ReaderSt : uint8_t {
    RS_IDLE,      ///< nothing on offer for it
    RS_PENDING,   ///< offered on its last fetch
    RS_SENDING,   ///< it asked for the package
    RS_FAILED,    ///< it reported a refusal (err)
};

struct Reader {
    uint32_t ip;          ///< 0 = free slot
    uint32_t seenMs;
    bool     knows;       ///< sent ?pkg= — a script that can update itself
    uint8_t  st;
    char     ver[kpkg::VER_CAP];
    char     err[24];
};

static const int KP_READERS = 4;

static Pkg    s_pkg;
static Reader s_rd[KP_READERS];

static const char PKG_TAR[]  = KINDLEPKG_DIR "/esp32dash.tar";
static const char PKG_OLD[]  = KINDLEPKG_DIR "/esp32dash.old";
static const char PKG_JSON[] = KINDLEPKG_DIR "/pkg.json";

// ============================================================================
// Files — every access under fsMutex (Pillar 1.3)
// ============================================================================

static bool saveMeta() {
    if (!sdAvailable) return false;
    JsonDocument d;
    d["ver"]      = (const char*)s_pkg.ver;
    d["size"]     = s_pkg.size;
    d["md5"]      = (const char*)s_pkg.md5;
    d["uploaded"] = s_pkg.uploaded;
    d["offer"]    = s_pkg.offer;
    return atomicWrite(*sdFs(), PKG_JSON,
                       [&d](File& f) { return serializeJson(d, static_cast<Print&>(f)) > 0; },
                       fsMutex);
}

void kindlePkgBegin() {
    memset(&s_pkg, 0, sizeof(s_pkg));
    memset(s_rd, 0, sizeof(s_rd));
    s_pkg.serial = 1;
    if (!sdAvailable) return;
    JsonDocument d;
    bool ok = false, tarThere = false;
    uint32_t tarSize = 0;
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        if (fsMutex && !g.isLocked()) return;
        File f = sdFs()->open(PKG_JSON, "r");
        if (f) {
            ok = !deserializeJsonFile(d, f);
            f.close();
        }
        File t = sdFs()->open(PKG_TAR, "r");
        if (t) {
            tarThere = true;
            tarSize  = (uint32_t)t.size();
            t.close();
        }
    }
    // Believed only when the file it describes is there, at the size it says:
    // a card edited on a PC must not have readers offered bytes that differ
    // from the MD5 they are told to expect. The reader checks the MD5 anyway,
    // so this is about not offering something that can only fail.
    const char* ver = d["ver"] | "";
    const char* md5 = d["md5"] | "";
    if (!ok || !tarThere || tarSize != (d["size"] | 0u) || !kpkg::verOk(ver) ||
        strlen(md5) != 32)
        return;
    s_pkg.have     = true;
    s_pkg.size     = tarSize;
    s_pkg.uploaded = d["uploaded"] | 0u;
    s_pkg.offer    = d["offer"] | false;
    strlcpy(s_pkg.ver, ver, sizeof(s_pkg.ver));
    strlcpy(s_pkg.md5, md5, sizeof(s_pkg.md5));
    Serial.printf("[kindlepkg] package %s (%u bytes)%s\n", s_pkg.ver,
                  (unsigned)s_pkg.size, s_pkg.offer ? ", offered" : "");
}

// ============================================================================
// Readers
// ============================================================================

/// The reader's IPv4 address as a number, 0 when there is none. Byte by byte
/// rather than IPAddress's conversion operator, which is not the same thing
/// on the two Arduino cores this builds against.
static uint32_t remoteIp(AsyncWebServerRequest* req) {
    AsyncClient* c = req->client();
    if (!c) return 0;
    const IPAddress a = c->remoteIP();
    return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

/// The reader at `ip`, else a free slot, else the one heard from longest ago.
static Reader& readerAt(uint32_t ip) {
    for (Reader& r : s_rd)
        if (r.ip == ip) return r;
    Reader* pick = nullptr;
    for (Reader& r : s_rd)
        if (!r.ip) { pick = &r; break; }
    if (!pick) {
        const uint32_t now = millis();
        pick = &s_rd[0];
        for (Reader& r : s_rd)
            if (now - r.seenMs > now - pick->seenMs) pick = &r;
    }
    memset(pick, 0, sizeof(*pick));
    pick->ip = ip;
    return *pick;
}

static Reader* readerFind(uint32_t ip) {
    for (Reader& r : s_rd)
        if (r.ip && r.ip == ip) return &r;
    return nullptr;
}

/// A short error word from the reader: [a-z0-9_], or nothing.
static void copyWord(char* dst, size_t cap, const String* v) {
    dst[0] = '\0';
    if (!v) return;
    size_t n = 0;
    for (size_t i = 0; i < v->length() && n + 1 < cap; i++) {
        const char c = (*v)[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) { dst[0] = '\0'; return; }
        dst[n++] = c;
    }
    dst[n] = '\0';
}

bool kindlePkgOffer(AsyncWebServerRequest* req, KindlePkgOffer& out) {
    const uint32_t ip = remoteIp(req);
    if (!ip) return false;
    Reader& r = readerAt(ip);
    r.seenMs = millis();

    const String* ver = queryArg(req, "pkg");
    r.knows = ver != nullptr;
    // "none" is a reader with no VERSION file — installed by hand from a
    // checkout. It can update itself, it just cannot say what it runs.
    if (ver && kpkg::verOk(ver->c_str())) strlcpy(r.ver, ver->c_str(), sizeof(r.ver));
    else r.ver[0] = '\0';

    const bool current = s_pkg.have && r.ver[0] && strcmp(r.ver, s_pkg.ver) == 0;
    if (!r.knows || !s_pkg.have || !s_pkg.offer || current) {
        r.st = RS_IDLE;
        r.err[0] = '\0';
        return false;
    }
    // A refusal is said on every fetch until the reader restarts and tries
    // again, with the MD5 of the package it refused (?pkgfor=). Counted only
    // when that is this package: the fetch that brings a new upload's offer
    // still carries the refusal of the one before it.
    char err[sizeof(r.err)];
    copyWord(err, sizeof(err), queryArg(req, "pkgerr"));
    const String* forMd5 = queryArg(req, "pkgfor");
    if (!forMd5 || strcmp(forMd5->c_str(), s_pkg.md5) != 0) err[0] = '\0';
    if (err[0]) {
        r.st = RS_FAILED;
        strlcpy(r.err, err, sizeof(r.err));
    } else if (r.st != RS_SENDING) {
        r.st = RS_PENDING;
        r.err[0] = '\0';
    }
    strlcpy(out.ver, s_pkg.ver, sizeof(out.ver));
    strlcpy(out.md5, s_pkg.md5, sizeof(out.md5));
    out.size = s_pkg.size;
    return true;
}

// ============================================================================
// GET / POST /api/kindle/pkg
// ============================================================================

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    if (!resp) { req->send(500); return; }
    resp->setCode(code);
    serializeJson(doc, static_cast<Print&>(*resp));
    req->send(resp);
}

static void sendErr(AsyncWebServerRequest* req, int code, const char* err) {
    JsonDocument out;
    out["ok"]    = false;
    out["error"] = err;
    sendJson(req, code, out);
}

static const char* readerStName(const Reader& r) {
    if (!r.knows) return "old";
    if (s_pkg.have && r.ver[0] && strcmp(r.ver, s_pkg.ver) == 0) return "current";
    switch (r.st) {
        case RS_PENDING: return "pending";
        case RS_SENDING: return "sending";
        case RS_FAILED:  return "failed";
        default:         return "idle";
    }
}

void handleKindlePkgGet(AsyncWebServerRequest* req) {
    JsonDocument out;
    out["sd"] = sdAvailable;
    if (s_pkg.have) {
        JsonObject p = out["pkg"].to<JsonObject>();
        p["ver"]      = (const char*)s_pkg.ver;
        p["size"]     = s_pkg.size;
        p["md5"]      = (const char*)s_pkg.md5;
        p["uploaded"] = s_pkg.uploaded;
    } else {
        out["pkg"] = nullptr;
    }
    out["offer"] = s_pkg.have && s_pkg.offer;
    JsonArray rs = out["readers"].to<JsonArray>();
    for (const Reader& r : s_rd) {
        if (!r.ip) continue;
        JsonObject o = rs.add<JsonObject>();
        char ip[16];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(r.ip >> 24),
                 (unsigned)(r.ip >> 16) & 255u, (unsigned)(r.ip >> 8) & 255u,
                 (unsigned)r.ip & 255u);
        o["ip"]   = ip;                  // char[]: copied
        o["ver"]  = (const char*)r.ver;
        o["seen"] = (uint32_t)(millis() - r.seenMs) / 1000u;
        o["st"]   = readerStName(r);
        if (r.st == RS_FAILED && r.err[0]) o["err"] = (const char*)r.err;
    }
    sendJson(req, 200, out);
}

void handleKindlePkgPost(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF
    if (!sdAvailable) { sendErr(req, 409, "no_sd"); return; }
    const String* a = formArg(req, "action");
    const String action = a ? *a : String();

    if (action == "offer" || action == "stop") {
        if (!s_pkg.have) { sendErr(req, 409, "no_pkg"); return; }
        s_pkg.offer = action == "offer";
        // A retry is an offer again: every reader that refused starts over.
        for (Reader& r : s_rd) { r.st = RS_IDLE; r.err[0] = '\0'; }
        if (!saveMeta()) { sendErr(req, 500, "write_failed"); return; }
    } else if (action == "delete") {
        {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
            if (fsMutex && !g.isLocked()) { sendErr(req, 503, "busy"); return; }
            // The serial moves under fsMutex, so a download half-way through
            // ends short rather than reading whatever comes next.
            s_pkg.have = false;
            s_pkg.offer = false;
            s_pkg.serial++;
            sdFs()->remove(PKG_TAR);
            sdFs()->remove(PKG_JSON);
        }
        for (Reader& r : s_rd) { r.st = RS_IDLE; r.err[0] = '\0'; }
    } else {
        sendErr(req, 400, "bad_request");
        return;
    }
    JsonDocument out;
    out["ok"] = true;
    sendJson(req, 200, out);
}

// ============================================================================
// GET /kindle/pkg.tar — the package, for a reader
// ============================================================================
// Read a chunk at a time under fsMutex, against the serial the response
// started on — the same reasons as /api/nodes/fw/bin: the file can be replaced
// or deleted from the page while a reader is half-way through it, and a
// replaced package ends the body short, which the reader's MD5 refuses.
//
// No token: a reader has none to send, and what is served here is the
// extension anyone can download from the repository. What it must not be is
// something the reader installs without checking, and it is not.

static int readChunk(uint32_t serial, uint32_t off, uint8_t* buf, size_t len) {
    if (!sdAvailable) return 0;
    MutexGuard g(fsMutex, pdMS_TO_TICKS(1000));
    if (fsMutex && !g.isLocked()) return -1;
    if (!s_pkg.have || s_pkg.serial != serial || off >= s_pkg.size) return 0;
    if (len > s_pkg.size - off) len = s_pkg.size - off;
    File f = sdFs()->open(PKG_TAR, "r");
    int n = 0;
    if (f && f.seek(off)) n = (int)f.read(buf, len);
    if (f) f.close();
    return n > 0 ? n : 0;
}

void handleKindlePkgTar(AsyncWebServerRequest* req) {
    if (!s_pkg.have) { req->send(404); return; }
    if (Reader* r = readerFind(remoteIp(req))) {
        if (r->st == RS_PENDING) r->st = RS_SENDING;
    }
    const uint32_t serial = s_pkg.serial;
    AsyncWebServerResponse* res = req->beginResponse(
        "application/x-tar", s_pkg.size,
        [serial](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            const int n = readChunk(serial, (uint32_t)index, buf, maxLen);
            return n < 0 ? RESPONSE_TRY_AGAIN : (size_t)n;
        });
    if (!res) { req->send(500); return; }
    res->addHeader("x-MD5", s_pkg.md5);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// ============================================================================
// POST /api/kindle/pkg/upload
// ============================================================================
// Streamed to KINDLEPKG_TMP a chunk at a time, fsMutex per write, with the
// MD5 and the package check (KindlePkgTar.h) run over the same bytes on the
// way past — the shape of /api/nodes/fw/upload. It becomes esp32dash.tar only
// when the whole file passed.

static volatile bool s_uploading = false;

struct PkgUpload {
    File           f;
    MD5Builder     md5;
    kpkg::TarScan  scan;
    uint32_t       size       = 0;
    bool           owner      = false;   ///< holds s_uploading
    bool           authFailed = false;
    int            code       = 0;
    const char*    err        = nullptr;

    PkgUpload() {
        md5.begin();
        kpkg::scanBegin(scan);
    }
    // Every way out of the request runs this — the request callback and the
    // disconnect cleaner both delete it — so the file is closed and the
    // upload slot released on all of them.
    ~PkgUpload() {
        if (f) {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            f.close();
        }
        if (owner) s_uploading = false;
    }
    void fail(int c, const char* e) {
        if (!err) { code = c; err = e; }
    }
};

void handleKindlePkgUpload(AsyncWebServerRequest* req, const String&, size_t index,
                           uint8_t* data, size_t len, bool) {
    if (index == 0) {
        if (req->_tempObject) { static_cast<PkgUpload*>(req->_tempObject)->fail(400, "bad_request"); return; }
        PkgUpload* u = new (std::nothrow) PkgUpload;
        req->_tempObject = u;
        if (!u) return;
        // Registered before anything can return early (ML-1 in WebServer.cpp).
        req->onDisconnect([req]() {
            delete static_cast<PkgUpload*>(req->_tempObject);
            req->_tempObject = nullptr;
        });
        if (!requireMutatingAuth(req)) { u->authFailed = true; return; }
        if (!sdAvailable) { u->fail(409, "no_sd"); return; }
        if (s_uploading) { u->fail(409, "busy"); return; }
        s_uploading = u->owner = true;
        MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
        if (fsMutex && !g.isLocked()) { u->fail(503, "busy"); return; }
        sdFs()->mkdir(KINDLEPKG_DIR);
        u->f = sdFs()->open(KINDLEPKG_TMP, FILE_WRITE);
        if (!u->f) u->fail(500, "write_failed");
    }
    PkgUpload* u = static_cast<PkgUpload*>(req->_tempObject);
    if (!u || u->authFailed || u->err || !len) return;
    kpkg::scanFeed(u->scan, data, len);
    // Refused on the first header that is wrong, rather than after writing a
    // megabyte of a file that was never going to be kept.
    if (u->scan.err) { u->fail(400, u->scan.err); return; }
    u->md5.add(data, (uint16_t)len);   // one TCP segment, far below 64 KB
    u->size += len;
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if ((fsMutex && !g.isLocked()) || u->f.write(data, len) != len) u->fail(500, "write_failed");
}

/// Make the checked KINDLEPKG_TMP the package. Not offered: the person
/// presses Offer once they have seen what was stored.
static bool commit(const char* ver, const char* md5, uint32_t size) {
    bool lost = false;
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
        if (fsMutex && !g.isLocked()) return false;
        const bool had = s_pkg.have;
        s_pkg.have = false;
        s_pkg.serial++;
        // FAT will not rename over a file: the old package steps aside, and
        // comes back if the new one cannot be put in place.
        sdFs()->remove(PKG_OLD);
        const bool moved = had && sdFs()->rename(PKG_TAR, PKG_OLD);
        if (!moved) sdFs()->remove(PKG_TAR);
        if (!sdFs()->rename(KINDLEPKG_TMP, PKG_TAR)) {
            sdFs()->remove(KINDLEPKG_TMP);
            if (moved && sdFs()->rename(PKG_OLD, PKG_TAR)) {
                s_pkg.have = true;            // the old package, untouched
                return false;
            }
            sdFs()->remove(PKG_JSON);
            lost = true;
        } else {
            sdFs()->remove(PKG_OLD);
        }
    }
    s_pkg.offer = false;
    for (Reader& r : s_rd) { r.st = RS_IDLE; r.err[0] = '\0'; }
    if (lost) return false;
    s_pkg.have = true;
    s_pkg.size = size;
    const uint32_t now = (uint32_t)time(nullptr);
    s_pkg.uploaded = now >= 1000000000u ? now : 0;
    strlcpy(s_pkg.ver, ver, sizeof(s_pkg.ver));
    strlcpy(s_pkg.md5, md5, sizeof(s_pkg.md5));
    saveMeta();
    return true;
}

void handleKindlePkgUploadDone(AsyncWebServerRequest* req) {
    PkgUpload* u = static_cast<PkgUpload*>(req->_tempObject);
    if (u && u->authFailed) return;   // 403 already sent
    if (!u) { req->send(500); return; }
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        u->f.close();                  // before the rename, or the remove
    }
    char ver[kpkg::VER_CAP] = "";
    char md5[33] = "";
    if (!u->err) {
        const char* why = kpkg::scanFinish(u->scan, ver, sizeof(ver));
        if (why) u->fail(400, why);
    }
    if (!u->err) {
        u->md5.calculate();
        u->md5.getChars(md5);
        if (!commit(ver, md5, u->size)) u->fail(500, "write_failed");
    }
    JsonDocument out;
    if (u->err) {
        out["ok"]    = false;
        out["error"] = u->err;
        if (u->owner) {                // ours: do not leave half a package behind
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (!fsMutex || g.isLocked()) sdFs()->remove(KINDLEPKG_TMP);
        }
    } else {
        out["ok"]   = true;
        out["ver"]  = (const char*)ver;
        out["size"] = u->size;
    }
    const int code = u->err ? u->code : 200;
    delete u;
    req->_tempObject = nullptr;
    sendJson(req, code, out);
}

#endif  // FEATURE_KINDLE_DASHBOARD
