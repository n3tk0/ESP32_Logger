#include "NodeFwApi.h"

#ifdef FEATURE_REMOTE_NODES

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <MD5Builder.h>
#include <mbedtls/sha256.h>
#include <new>

#include "IngestHandler.h"              // ingestAuthorised()
#include "NodeCfgApi.h"                 // nodesApiBody() / nodesApiSend()
#include "RequireAuth.h"
#include "../core/Globals.h"            // sdAvailable
#include "../core/SdCompat.h"           // sdFs()
#include "../nodes/NodeFwStore.h"
#include "../pipeline/DataPipeline.h"   // fsMutex
#include "../utils/MutexGuard.h"

// ============================================================================
// GET / POST /api/nodes/fw
// ============================================================================

void handleNodesFwGet(AsyncWebServerRequest* req) {
    JsonDocument out;
    nodeFwApiGet(out);
    nodesApiSend(req, 200, out);
}

static void nodesFwPost(AsyncWebServerRequest* req, JsonDocument& body) {
    JsonDocument out;
    nodesApiSend(req, nodeFwApiPost(body.as<JsonObjectConst>(), out), out);
}

void handleNodesFwBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total) {
    nodesApiBody(req, data, len, index, total, nodesFwPost);
}

// ============================================================================
// GET /api/nodes/fw/bin — the image, for a WiFi node (§3)
// ============================================================================
// Not AsyncFileResponse, which is how /download serves the card: that holds
// one File open for the whole response and reads it from the async task with
// no lock at all. Here the file can also be replaced or deleted from the Nodes
// page while a node is half-way through it, so every chunk is read by
// nodeFwRead() — under fsMutex, against the serial of the image the response
// started on. A replaced image ends the body short, and the node's
// Update.end() refuses the short (or MD5-mismatched) image; a card busy for a
// second is RESPONSE_TRY_AGAIN, and the library calls back on the next poll.

void handleNodesFwBin(AsyncWebServerRequest* req) {
    if (!ingestAuthorised(req)) { req->send(401); return; }
    const AsyncWebParameter* p = req->getParam("kind");
    const nodefw::Kind k = nodefw::kindFromName(p ? p->value().c_str() : "");
    NodeFwImage im;
    if (!nodeFwImage(k, im)) { req->send(503); return; }
    if (!im.size) { req->send(404); return; }
    const uint32_t serial = im.serial;
    AsyncWebServerResponse* r = req->beginResponse(
        "application/octet-stream", im.size,
        [k, serial](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            const int n = nodeFwRead(k, serial, (uint32_t)index, buf, maxLen);
            return n < 0 ? RESPONSE_TRY_AGAIN : (size_t)n;
        });
    if (!r) { req->send(500); return; }
    r->addHeader("x-MD5", im.md5);
    req->send(r);
}

// ============================================================================
// POST /api/nodes/fw/upload — §2.1
// ============================================================================
// Streamed to NODEFW_TMP one chunk at a time, like /upload: fsMutex is taken
// per write, never for the whole body (a C3 image is ~1 MB and StorageTask
// must keep logging). MD5, SHA-256 and the marker scan run over the same
// bytes on the way past, and the first HEAD_NEED bytes are kept for the header
// checks — so the image is judged without being read back. It becomes
// <kind>.bin only once every check passed (nodeFwCommit).
//
// One upload at a time: there is one upload.tmp.
static volatile bool s_uploading = false;

struct FwUpload {
    File                   f;
    MD5Builder             md5;
    mbedtls_sha256_context sha;
    nodefw::MarkerScan     scan;
    uint8_t                head[nodefw::HEAD_NEED];
    uint32_t               size       = 0;
    bool                   owner      = false;   ///< holds s_uploading
    bool                   authFailed = false;
    int                    code       = 0;       ///< first failure: HTTP status...
    const char*            err        = nullptr; ///< ...and its error

    FwUpload() {
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);          // 0 = SHA-256, not -224
        md5.begin();
        nodefw::scanBegin(scan);
    }
    // Every way out of the request runs this — the request callback deletes
    // it, the disconnect cleaner deletes it on an abort — so the file is
    // closed, the hash engine freed and the upload slot released on all of
    // them. A leftover upload.tmp is harmless: the next upload truncates it.
    ~FwUpload() {
        mbedtls_sha256_free(&sha);
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

static void hex(char* out, const uint8_t* b, size_t n) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = '\0';
}

void handleNodesFwUpload(AsyncWebServerRequest* req, const String&, size_t index,
                         uint8_t* data, size_t len, bool) {
    if (index == 0) {
        // A second file part would restart the hashes over half an image.
        if (req->_tempObject) { static_cast<FwUpload*>(req->_tempObject)->fail(400, "bad_request"); return; }
        FwUpload* u = new (std::nothrow) FwUpload;
        req->_tempObject = u;
        if (!u) return;
        // Registered before anything can return early (ML-1 in WebServer.cpp).
        req->onDisconnect([req]() {
            delete static_cast<FwUpload*>(req->_tempObject);
            req->_tempObject = nullptr;
        });
        // Here and not in the request callback, which runs only after the
        // whole body has been written to the card.
        if (!requireMutatingAuth(req)) { u->authFailed = true; return; }
        if (!sdAvailable) { u->fail(409, "no_sd"); return; }
        if (s_uploading) { u->fail(409, "busy"); return; }
        s_uploading = u->owner = true;
        MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
        if (fsMutex && !g.isLocked()) { u->fail(503, "busy"); return; }
        sdFs()->mkdir(NODEFW_DIR);
        u->f = sdFs()->open(NODEFW_TMP, FILE_WRITE);
        if (!u->f) u->fail(500, "write_failed");
    }
    FwUpload* u = static_cast<FwUpload*>(req->_tempObject);
    if (!u || u->authFailed || u->err || !len) return;
    // Past the largest slot of any kind it is refused whatever it turns out
    // to be; stop writing it now rather than fill the card with it.
    if ((uint64_t)u->size + len > EN_FW_MAX_SIZE) { u->fail(400, "too_big"); return; }
    if (u->size < nodefw::HEAD_NEED) {
        const size_t k = len < nodefw::HEAD_NEED - u->size ? len : nodefw::HEAD_NEED - u->size;
        memcpy(u->head + u->size, data, k);
    }
    u->md5.add(data, (uint16_t)len);   // one TCP segment, far below 64 KB
    mbedtls_sha256_update(&u->sha, data, len);
    nodefw::scanFeed(u->scan, data, len);
    u->size += len;
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if ((fsMutex && !g.isLocked()) || u->f.write(data, len) != len) u->fail(500, "write_failed");
}

void handleNodesFwUploadDone(AsyncWebServerRequest* req) {
    FwUpload* u = static_cast<FwUpload*>(req->_tempObject);
    if (u && u->authFailed) return;   // 403 already sent
    NodeFwMeta m;
    memset(&m, 0, sizeof(m));
    if (!u) {
        req->send(500);               // no file part, or out of memory
        return;
    }
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        u->f.close();                        // before the rename, or the remove
    }
    if (!u->err) {
        m.kind = nodefw::scanKind(u->scan);
        m.size = u->size;
        const size_t hn = u->size < nodefw::HEAD_NEED ? u->size : nodefw::HEAD_NEED;
        // §1: the marker says which of our firmwares it is; its slot says how
        // big it may be; its header must look like one; a C3 image's id is
        // what the node compares, and 0 is EN_FW_ANY on the wire.
        if (m.kind == nodefw::KIND_NONE)                       u->fail(400, "not_node_image");
        else if (m.size > nodefw::maxSize(m.kind))             u->fail(400, "too_big");
        else if (!nodefw::headOk(m.kind, u->head, hn))         u->fail(400, "bad_header");
        else if (m.kind == nodefw::KIND_ESPNOW_C3 && !(m.imgId = nodefw::c3ImageId(u->head)))
                                                               u->fail(400, "zero_id");
    }
    if (!u->err) {
        uint8_t d[32];
        u->md5.calculate();
        u->md5.getChars(m.md5);
        mbedtls_sha256_finish(&u->sha, d);
        hex(m.sha256, d, 32);
        memcpy(m.ver, u->scan.ver, sizeof(m.ver));
        if (!nodeFwCommit(m)) u->fail(500, "write_failed");
    }
    JsonDocument out;
    if (u->err) {
        out["ok"]    = false;
        out["error"] = u->err;
        if (u->owner) {                      // ours: do not leave 1 MB of it behind
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (!fsMutex || g.isLocked()) sdFs()->remove(NODEFW_TMP);
        }
    } else {
        out["ok"]   = true;
        out["kind"] = nodefw::kindName(m.kind);
        out["ver"]  = (const char*)m.ver;
        out["size"] = m.size;
    }
    const int code = u->err ? u->code : 200;
    delete u;
    req->_tempObject = nullptr;
    nodesApiSend(req, code, out);
}

#endif  // FEATURE_REMOTE_NODES
