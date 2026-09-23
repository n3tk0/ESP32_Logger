#include "NodeCfgApi.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <new>

#include "IngestHandler.h"                  // INGEST_TOKEN, REMOTE_STATUS_STALE_MS
#include "RequireAuth.h"
#include "WebServer.h"                      // applyNetworkForm()
#include "../core/Globals.h"                // config, shouldRestart
#include "../managers/ConfigManager.h"      // saveConfig()
#include "../nodecfg/UdpDiscovery.h"
#include "../nodecfg/UdpDiscoveryHmac.h"
#include "../nodes/NodeCfgStore.h"
#include "../sensors/RemoteIngest.h"
#include "../utils/JsonResponse.h"
#ifdef FEATURE_ESPNOW_INGEST
#include "../espnow/EspNowIngest.h"
#endif

// ============================================================================
// Bodies
// ============================================================================
// A partial config is small, but a whole one — eight sensors, every field —
// is ~1.5 KB of JSON and arrives in more than one TCP segment, so the body is
// accumulated through _tempObject like /api/ingest and /api/kindle/slots.
static constexpr size_t NODES_MAX_BODY = 3072;

typedef void (*JsonBodyFn)(AsyncWebServerRequest* req, JsonDocument& body);

static void accumulate(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total, JsonBodyFn fn) {
    if (total > NODES_MAX_BODY) {
        if (index == 0)
            req->send(413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
        return;
    }
    if (index == 0) {
        req->_tempObject = new (std::nothrow) String();
        if (!req->_tempObject) {
            req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
            return;
        }
        // Registered straight after the allocation: a client that goes away
        // mid-body would otherwise orphan the buffer.
        req->onDisconnect([req]() {
            delete static_cast<String*>(req->_tempObject);
            req->_tempObject = nullptr;
        });
        static_cast<String*>(req->_tempObject)->reserve(total);
    }
    String* buf = static_cast<String*>(req->_tempObject);
    if (!buf) return;
    buf->concat(reinterpret_cast<const char*>(data), len);
    if (index + len < total) return;

    if (requireMutatingAuth(req)) {         // rate limit + CSRF, once, on the whole body
        JsonDocument body;
        if (deserializeJson(body, buf->c_str(), buf->length()) || !body.is<JsonObject>())
            req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        else
            fn(req, body);
    }
    delete buf;
    req->_tempObject = nullptr;
}

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    if (code == 200) { sendJsonResponse(req, doc); return; }
    String s;
    serializeJson(doc, s);
    req->send(code, "application/json", s);
}

// ============================================================================
// Which nodes exist
// ============================================================================

/// Is this a node one of the status lists shows? (A node without a config
/// file is answered with nulls rather than a 404 — §7.)
static bool nodeListed(bool espnow, const char* name, uint8_t id) {
    if (espnow) {
#ifdef FEATURE_ESPNOW_INGEST
        EspNowNode nodes[ESPNOW_MAX_NODES];
        const int n = espnowCopyNodes(nodes, ESPNOW_MAX_NODES);
        for (int i = 0; i < n; i++) if (nodes[i].nodeId == id) return true;
#endif
        return false;
    }
    char nid[RemoteIngest::MAX_NODE_ID];
    for (int i = 0; remoteIngest.nodeIdAt(i, nid, sizeof(nid)); i++)
        if (strcmp(nid, name) == 0) return true;
    return false;
}

// ============================================================================
// /api/nodes/config
// ============================================================================

void handleNodesConfigGet(AsyncWebServerRequest* req) {
    char key[ncr::KEY_CAP] = "";
    if (req->hasParam("key")) nodecfg::copyStr(key, sizeof(key), req->getParam("key")->value().c_str());
    bool espnow; char name[nodecfg::NODE_NAME_MAX + 1]; uint8_t id;
    const bool known = ncr::parseKey(key, espnow, name, id) && nodeListed(espnow, name, id);
    JsonDocument out;
    sendJson(req, nodeCfgApiGet(key, known, out), out);
}

static void nodesConfigPost(AsyncWebServerRequest* req, JsonDocument& body) {
    JsonDocument out;
    sendJson(req, nodeCfgApiPost(body.as<JsonObjectConst>(), out), out);
}

void handleNodesConfigBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                           size_t index, size_t total) {
    accumulate(req, data, len, index, total, nodesConfigPost);
}

// ============================================================================
// /api/nodes/handover — §4
// ============================================================================

/// Every node that could be asked to follow: those with a config file, and
/// those in either status list without one (they cannot follow, and say so by
/// never becoming ready). Classified ready / pending / offline by the rules in
/// NodeCfgRules.h and each transport's own offline rule. Returns how many are
/// pending; fills `out` with the three lists when it is not null.
static int handoverSort(JsonObject out) {
    static const int MAXK = NODECFG_LIST_MAX;
    NodeCfgKey keys[MAXK];
    bool       offline[MAXK];
    int n = nodeCfgKeys(keys, MAXK);
    auto has = [&](bool espnow, const char* name, uint8_t id) {
        for (int i = 0; i < n; i++)
            if (keys[i].espnow == espnow &&
                (espnow ? keys[i].id == id : strcmp(keys[i].name, name) == 0)) return i;
        return -1;
    };
    for (int i = 0; i < n; i++) offline[i] = true;   // until a list says otherwise

#ifdef FEATURE_ESPNOW_INGEST
    {
        EspNowNode nodes[ESPNOW_MAX_NODES];
        const int      c   = espnowCopyNodes(nodes, ESPNOW_MAX_NODES);
        const uint32_t now = millis();
        const uint8_t  iv  = espnowGetOfflineIntervals();
        for (int j = 0; j < c; j++) {
            int i = has(true, "", nodes[j].nodeId);
            if (i < 0 && n < MAXK) {
                i = n++;
                keys[i].espnow = true; keys[i].id = nodes[j].nodeId; keys[i].name[0] = '\0';
            }
            if (i >= 0) offline[i] = espnowNodeOffline(nodes[j], now, iv);
        }
    }
#endif
    {
        char nid[RemoteIngest::MAX_NODE_ID];
        for (int j = 0; remoteIngest.nodeIdAt(j, nid, sizeof(nid)); j++) {
            int i = has(false, nid, 0);
            if (i < 0 && n < MAXK) {
                i = n++;
                keys[i].espnow = false; keys[i].id = 0;
                nodecfg::copyStr(keys[i].name, sizeof(keys[i].name), nid);
            }
            const uint32_t age = remoteIngest.ageMsForNode(nid);
            if (i >= 0) offline[i] = !(age != UINT32_MAX && age < REMOTE_STATUS_STALE_MS);
        }
    }

    JsonArray lists[3];
    if (!out.isNull()) {
        lists[ncr::HO_READY]   = out["ready"].to<JsonArray>();
        lists[ncr::HO_PENDING] = out["pending"].to<JsonArray>();
        lists[ncr::HO_OFFLINE] = out["offline"].to<JsonArray>();
    }
    int pending = 0;
    for (int i = 0; i < n; i++) {
        NodeCfgSummary s;
        const bool have = nodeCfgSummary(keys[i].espnow, keys[i].name, keys[i].id, s);
        const uint8_t c = ncr::hoClassify(have, have ? s.applied : 0, have ? s.hoRev : 0, offline[i]);
        if (c == ncr::HO_PENDING) pending++;
        if (out.isNull()) continue;
        char key[ncr::KEY_CAP];
        ncr::formatKey(key, keys[i].espnow, keys[i].name, keys[i].id);
        lists[c].add((const char*)key);
    }
    return pending;
}

void handleNodesHandoverGet(AsyncWebServerRequest* req) {
    JsonDocument out;
    const bool active = nodeCfgHandoverActive();
    out["active"] = active;
    if (active) {
        char ssid[nodecfg::SSID_CAP];
        nodeCfgHandoverSsid(ssid);
        out["ssid"] = (const char*)ssid;
        handoverSort(out.as<JsonObject>());
    }
    sendJsonResponse(req, out);
}

static const char* formField(void* ctx, const char* key) {
    JsonVariantConst v = (*static_cast<JsonObjectConst*>(ctx))[key];
    return v.is<const char*>() ? v.as<const char*>() : nullptr;
}

/// §4 step 4: save the collector's own network config the way /save_network
/// does — the whole form when the page sent one, else only the SSID and
/// passphrase — and restart into it, exactly as that path does.
static bool handoverSwitch() {
    JsonDocument h;
    if (!nodeCfgHandoverFinish(h)) return false;
    NetworkConfig net = config.network;
    JsonObjectConst form = h["form"];
    if (!form.isNull()) applyNetworkForm(net, formField, &form);   // checked at start
    // The network the nodes were told about is the one joined, whatever the
    // form's own copy says.
    strlcpy(net.clientSSID, h["ssid"] | "", sizeof(net.clientSSID));
    strlcpy(net.clientPassword, h["pass"] | "", sizeof(net.clientPassword));
    config.network = net;
    saveConfig();
    Serial.printf("[nodecfg] handover: switching to \"%s\"\n", net.clientSSID);
    shouldRestart = true;
    restartTimer  = millis();
    return true;
}

static void handoverFail(AsyncWebServerRequest* req, int code, const char* reason) {
    JsonDocument out;
    out["ok"]     = false;
    out["reason"] = reason;
    sendJson(req, code, out);
}

static void nodesHandoverPost(AsyncWebServerRequest* req, JsonDocument& body) {
    const char* action = body["action"] | "";
    if (strcmp(action, "start") == 0) {
        const char* ssid = body["ssid"] | "";
        const char* pass = body["pass"] | "";
        const size_t sl = strlen(ssid), pl = strlen(pass);
        if (sl == 0 || sl >= nodecfg::SSID_CAP) { handoverFail(req, 400, "ssid is required (1-32 characters)"); return; }
        // What a WiFi node's validator (§1.2 net.next.pass) would refuse, refused
        // here, before every node is handed it.
        if (pl != 0 && (pl < 8 || pl > 63)) { handoverFail(req, 400, "pass must be 8-63 characters, or empty for an open network"); return; }
        JsonVariantConst form = body["form"];
        if (!form.isNull()) {
            JsonObjectConst fo = form.as<JsonObjectConst>();
            NetworkConfig scratch = config.network;
            const char* err = fo.isNull() ? "form must be an object"
                                          : applyNetworkForm(scratch, formField, &fo);
            if (err) { handoverFail(req, 400, err); return; }
        }
        if (!nodeCfgHandoverStart(ssid, pass, form)) { handoverFail(req, 500, "could not save the handover"); return; }
    } else if (strcmp(action, "switch") == 0) {
        if (!handoverSwitch()) { handoverFail(req, 409, "no handover in progress"); return; }
    } else if (strcmp(action, "cancel") == 0) {
        if (!nodeCfgHandoverCancel()) { handoverFail(req, 503, "busy, try again"); return; }
    } else {
        handoverFail(req, 400, "unknown action");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

void handleNodesHandoverBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                             size_t index, size_t total) {
    accumulate(req, data, len, index, total, nodesHandoverPost);
}

// ============================================================================
// loop()
// ============================================================================

/// §3.1: answer a signed "ESPL?" broadcast with our HTTP port, so a WiFi node
/// whose configured host stopped answering (or that just changed network)
/// finds the collector again. A plain non-blocking lwIP socket polled from
/// loop(): no task, no library, a few queries a day.
static int      s_udp       = -1;
static uint32_t s_udpRetry  = 0;

static void discoveryTick() {
    if (s_udp < 0) {
        if (WiFi.getMode() == WIFI_OFF || (int32_t)(millis() - s_udpRetry) < 0) return;
        s_udpRetry = millis() + 5000;
        const int s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) return;
        const int one = 1;
        lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        sockaddr_in a = {};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(nodecfg::udpdisc::DISCOVERY_PORT);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (lwip_bind(s, (sockaddr*)&a, sizeof(a)) != 0) { lwip_close(s); return; }
        s_udp = s;
    }
    namespace ud = nodecfg::udpdisc;
    for (int k = 0; k < 4; k++) {                // a few per pass; the rest wait
        uint8_t     buf[ud::QUERY_LEN + 8];
        sockaddr_in from;
        socklen_t   fl = sizeof(from);
        const int n = lwip_recvfrom(s_udp, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&from, &fl);
        if (n <= 0) break;
        uint8_t nonce[ud::NONCE_LEN];
        char    name[ud::NAME_LEN + 1];
        if (!ud::parseQuery(buf, (size_t)n, INGEST_TOKEN, ud::udpdiscHmacSha256, nonce, name))
            continue;
        uint8_t reply[ud::REPLY_LEN];
        // Port 80: the web server (and /api/ingest) is AsyncWebServer server(80).
        if (ud::buildReply(reply, nonce, 80, INGEST_TOKEN, ud::udpdiscHmacSha256))
            lwip_sendto(s_udp, reply, sizeof(reply), 0, (sockaddr*)&from, fl);
    }
}

void nodeCfgApiTick() {
    discoveryTick();

    // §4 step 4: switch by itself once no node that can follow is still
    // pending. Checked every couple of seconds, not every pass.
    static uint32_t s_nextHo = 0;
    if ((int32_t)(millis() - s_nextHo) < 0) return;
    s_nextHo = millis() + 2000;
    if (shouldRestart || !nodeCfgHandoverActive()) return;
    if (ncr::hoAutoSwitch(true, handoverSort(JsonObject()), millis() - nodeCfgHandoverStartedMs()))
        handoverSwitch();
}

#endif  // FEATURE_REMOTE_NODES
