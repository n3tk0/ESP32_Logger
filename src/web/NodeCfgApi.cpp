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

// ============================================================================
// Bodies
// ============================================================================
// A partial config is small, but a whole one — eight sensors, every field —
// is ~1.5 KB of JSON and arrives in more than one TCP segment, so the body is
// accumulated through _tempObject by /api/ingest's accumulateBody().
static constexpr size_t NODES_MAX_BODY = 3072;

typedef void (*JsonBodyFn)(AsyncWebServerRequest* req, JsonDocument& body);

static void accumulate(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total, JsonBodyFn fn) {
    String* buf = accumulateBody(req, data, len, index, total, NODES_MAX_BODY);
    if (!buf) return;
    if (requireMutatingAuth(req)) {         // rate limit + CSRF, once, on the whole body
        JsonDocument body;
        if (deserializeJson(body, buf->c_str(), buf->length()) || !body.is<JsonObject>())
            req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        else
            fn(req, body);
    }
    delete buf;
}

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    if (!resp) { req->send(500); return; }
    resp->setCode(code);
    serializeJson(doc, *resp);
    req->send(resp);
}

// ============================================================================
// /api/nodes/config
// ============================================================================

void handleNodesConfigGet(AsyncWebServerRequest* req) {
    const AsyncWebParameter* p = req->getParam("key");
    JsonDocument out;
    sendJson(req, nodeCfgApiGet(p ? p->value().c_str() : "", out), out);
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

void handleNodesHandoverGet(AsyncWebServerRequest* req) {
    JsonDocument out;
    char ssid[nodecfg::SSID_CAP];
    const bool active = nodeCfgHandover(ssid, nullptr);
    out["active"] = active;
    if (active) {
        out["ssid"] = (const char*)ssid;
        nodeCfgHandoverSort(out.as<JsonObject>());
    }
    sendJson(req, 200, out);
}

static const char* formField(void* ctx, const char* key) {
    // as<const char*>() is null for anything that is not a string.
    return (*static_cast<JsonObjectConst*>(ctx))[key].as<const char*>();
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

/// Every reason is a fixed string of ours or applyNetworkForm()'s, none with
/// a quote or backslash in it, so no JSON escaping is needed.
static void handoverFail(AsyncWebServerRequest* req, int code, const char* reason) {
    char b[128];
    snprintf(b, sizeof(b), "{\"ok\":false,\"reason\":\"%s\"}", reason);
    req->send(code, "application/json", b);
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
            // All strings, as the form posts them (§4.1): formField() reads
            // any other value as absent, so a `"useStaticIP": true` would be
            // dropped at the switch, not refused here.
            for (JsonPairConst kv : fo)
                if (!kv.value().is<const char*>()) err = "form values must be strings";
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
    uint32_t since = 0;
    if (shouldRestart || !nodeCfgHandover(nullptr, &since)) return;
    if (ncr::hoAutoSwitch(true, nodeCfgHandoverSort(JsonObject()), millis() - since)) handoverSwitch();
}

#endif  // FEATURE_REMOTE_NODES
