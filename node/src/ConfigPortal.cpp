#include "ConfigPortal.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <new>

#include "NodeStore.h"
#include "NodeLog.h"
#include "node_common/NodeSensors.h"
#include "src/nodecfg/NodeConfigJson.h"
#include "NodeCfgTables.h"
#include "src/nodecfg/NodePortalPage.h"

using nodecfg::NodeConfig;

// The sync web server is the right shape here: the page is a handful of
// requests from one phone, the AP portal runs only while nothing else is
// happening, and blocking inside a handler costs nothing when there is no
// sensor loop to stall.
static ESP8266WebServer        s_http(80);
static DNSServer               s_dns;
static NodeConfig*             s_target  = nullptr;
static const PortalLinkStatus* s_link    = nullptr;
static bool                    s_saved   = false;
static uint32_t                s_savedAt = 0;

/// §6: "{"ok":true} then restart after 500 ms". Long enough for the reply to
/// leave; the page then polls /api/status for the new uptime.
static const uint32_t kRestartDelayMs = 500;

/// True while the page is being served on the STA interface — the home LAN —
/// rather than on the node's own access point.
///
/// THE DIFFERENCE IS WHO CAN REACH IT. To open the AP portal you have to be
/// associated with the node's access point, standing next to it, during a
/// window it opens only after repeated WiFi failures. The background server
/// is on the LAN, for the node's whole uptime, reachable by anything on the
/// network — so every route there sits behind basic auth.
static bool s_background = false;

/// Handlers are registered once for the life of the process.
///
/// ESP8266WebServer::on() appends to a list and never deduplicates, and
/// portalStartBackground() is called on every transition back to connected —
/// so a node whose router reboots nightly grew dead handler entries a night,
/// walked on every request, until it ran out of heap.
static bool s_routesBound = false;

void portalSetLinkStatus(const PortalLinkStatus* s) { s_link = s; }

/// Basic auth on the background server, using the credentials the node
/// already holds for the collector.
///
/// Reusing them is not elegant, but the alternative was worse in both
/// directions: a new setting nobody would fill in, or a page on the LAN that
/// anyone could POST to, repointing the node at their own collector and
/// restarting it. The AP portal is not gated, because reaching it already
/// requires the AP's own password.
static bool authOk() {
    if (!s_background) return true;
    const NodeConfig& c = *s_target;
    if (c.net.basic_user[0] && c.net.basic_pass[0] &&
        s_http.authenticate(c.net.basic_user, c.net.basic_pass))
        return true;
    s_http.requestAuthentication();
    return false;
}

bool portalButtonHeld() {
    pinMode(PORTAL_TRIGGER_PIN, INPUT_PULLUP);
    // One read can catch a floating pin mid-transition; require the button to
    // still be down after a debounce interval.
    if (digitalRead(PORTAL_TRIGGER_PIN) != LOW) return false;
    delay(50);
    return digitalRead(PORTAL_TRIGGER_PIN) == LOW;
}

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------

/// Serialise into a String and let the document go BEFORE sending: on a part
/// with ~40 KB of heap the JsonDocument and the socket buffers should not
/// both be alive at the peak. The caller builds the document in a scope that
/// ends before this returns.
static void sendJson(int code, const String& body) {
    s_http.sendHeader("Cache-Control", "no-store");
    s_http.send(code, "application/json", body);
}

static void sendError(int code, const char* field, const char* reason) {
    String out;
    {
        JsonDocument doc;
        doc["ok"]     = false;
        doc["field"]  = field;
        doc["reason"] = reason;
        serializeJson(doc, out);
    }
    sendJson(code, out);
}

// ---------------------------------------------------------------------------
// GET / — the page
// ---------------------------------------------------------------------------

/// The gzip bytes exactly as tools/build_node_portal.py wrote them, streamed
/// out of flash by send_P in small chunks — never copied into RAM whole. The
/// browser inflates it; every browser since 2000 does.
static void handlePage() {
    if (!authOk()) return;
    s_http.sendHeader("Content-Encoding", "gzip");
    s_http.sendHeader("Cache-Control", "no-store");
    s_http.send_P(200, PSTR("text/html; charset=utf-8"),
                  (PGM_P)NODE_PORTAL_GZ, sizeof(NODE_PORTAL_GZ));
}

// ---------------------------------------------------------------------------
// GET /api/config
// ---------------------------------------------------------------------------

static void handleConfigGet() {
    if (!authOk()) return;
    String out;
    {
        JsonDocument doc;
        // No NCJ_SECRETS: the passphrase, token and basic-auth password go
        // out as "" plus "<field>_set", here and everywhere a GET answers.
        nodecfg::encodeConfig(*s_target, doc["config"].to<JsonObject>(), 0);
        nodeEncodeCaps(doc["caps"].to<JsonObject>());
        if (doc.overflowed()) {
            sendError(500, "", "out of memory");
            return;
        }
        out.reserve(measureJson(doc) + 1);
        serializeJson(doc, out);
    }
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
// POST /api/config
// ---------------------------------------------------------------------------

/// What a POST decodes into: a whole candidate config and the validator's
/// answer — about 1.5 KB together, so on the heap for the length of the
/// request rather than on the 4 KB loop stack or in static RAM for ever.
struct PostWork {
    NodeConfig            cfg;
    nodecfg::Validation   v;
    nodecfg::Issue        issue;
};

static void handleConfigPost() {
    // Checked HERE too, not only on the page. A POST does not have to come
    // from a page this device served, and this is the handler that rewrites
    // the node's collector address and then restarts it.
    if (!authOk()) return;

    PostWork* w = new (std::nothrow) PostWork();
    if (!w) {
        sendError(500, "", "out of memory");
        return;
    }
    // Decode onto a COPY. Writing straight into the running config would
    // leave it half-applied on the refusal paths below, and the node would run
    // on a config that is neither the old one nor the new one until a reboot.
    w->cfg = *s_target;
    bool decoded;
    {
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, s_http.arg("plain"));
        if (err || !doc.is<JsonObject>()) {
            delete w;
            sendError(400, "", "not a JSON object");
            return;
        }
        // No NCJ_DEC_REV: a page echoing a stale rev must not overwrite the
        // real one, and transport/hw/fw are the firmware's to say.
        decoded = nodecfg::decodeConfig(doc, w->cfg, 0, &w->issue);
    }
    if (!decoded) {
        LOGF("[portal] refused %s: %s\n", w->issue.field, w->issue.reason);
        sendError(400, w->issue.field, w->issue.reason);
        delete w;
        return;
    }

    nodeValidate(w->cfg, w->v);
    if (!w->v.ok) {
        LOGF("[portal] refused %s: %s\n", w->v.error.field, w->v.error.reason);
        String out;
        {
            JsonDocument doc;
            nodecfg::encodeValidation(w->v, doc.to<JsonObject>());
            serializeJson(doc, out);
        }
        delete w;
        sendJson(400, out);
        return;
    }

    // §0.4: a change made here wins. `local` makes the node report the whole
    // config on its next POST; the collector adopts it and confirms the rev.
    w->cfg.local = true;
    if (!storeSave(w->cfg, true)) {
        delete w;
        sendError(500, "", "could not write to the filesystem");
        return;
    }
    // A network change the collector sent may be on trial (§4.7). Whoever is
    // at this page has taken over: rolling back underneath them would throw
    // away what they just typed.
    {
        SyncState ss;
        syncLoad(ss);
        if (ss.trialRev) {
            ss.trialRev = 0;
            syncSave(ss);
        }
    }

    *s_target = w->cfg;
    String out;
    {
        JsonDocument doc;
        nodecfg::encodeValidation(w->v, doc.to<JsonObject>());   // ok + any warnings
        serializeJson(doc, out);
    }
    delete w;
    sendJson(200, out);
    s_saved   = true;
    s_savedAt = millis();
    LOGLN("[portal] saved; restarting");
}

// ---------------------------------------------------------------------------
// GET /api/scan — the network picker
// ---------------------------------------------------------------------------
//
// One endpoint that starts a scan and reports it, polled by the page, for the
// reason the collector splits its scan the same way: an ESP8266 scan takes
// two seconds or more, and doing it inside the request would hold the only
// web-server thread the portal has — on the AP, that is the phone's own
// connection timing out. scanNetworks(true) returns immediately.

/// True while the scan has the radio widened from AP to AP+STA, so the mode
/// can be put back. The ESP8266 has ONE radio: the softAP and the station
/// share a channel, and a station that associates drags the AP onto its
/// channel — disassociating the phone that is standing in the portal. Leaving
/// AP_STA on for the rest of the session leaves that trap armed.
static bool     s_widenedForScan = false;
static bool     s_scanPending    = false;   ///< results belong to a scan we started
static uint32_t s_scanStartedMs  = 0;

/// How long the radio may stay widened before the portal narrows it itself.
/// An ESP8266 scan is two to four seconds; the page polls every second or
/// so, so nothing legitimate is still waiting at twenty. Well clear of a slow
/// scan, well inside a session.
static const uint32_t kScanCeilingMs = 20000;

/// Put the radio back the way the portal found it, once the scan is over.
static void narrowAfterScan() {
    if (!s_widenedForScan) return;
    s_widenedForScan = false;
    WiFi.mode(WIFI_AP);
}

/// THE BROWSER IS THE HALF OF THIS THAT CAN WALK AWAY.
///
/// narrowAfterScan() runs on the /api/scan poll, which is the page's job.
/// Close the tab, let its own retry counter give up, drop the link, or lock
/// the phone — the poll stops, the scan finishes into a buffer nobody reads,
/// and the radio stays in AP_STA for the rest of the portal session with
/// exactly the trap armed that narrowAfterScan() exists to disarm.
///
/// So the portal narrows on a ceiling of its own rather than on being asked.
/// Time and not completion, deliberately: changing mode is entitled to drop
/// the scan results, and narrowing the moment the scan finished would race
/// the poll that is about to collect them.
static void scanWatchdog() {
    if (!s_widenedForScan) return;
    if ((millis() - s_scanStartedMs) > kScanCeilingMs) {
        LOGLN("[portal] scan abandoned; putting the radio back to AP");
        s_scanPending = false;
        narrowAfterScan();
    }
}

static void startScan() {
    // Widen AP → AP_STA so the access point the phone is sitting on stays up
    // while the radio scans. On the LAN the node is already in STA and can
    // scan as it is.
    if (WiFi.getMode() == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
        s_widenedForScan = true;
        // Scanning does not need an association, and an association is the
        // thing that would move the AP's channel. portalRun() can be reached
        // from setup() before anything calls WiFi.disconnect(), so the SDK
        // may still hold credentials from flash and auto-connect on its own
        // the moment the station interface comes up.
        WiFi.disconnect(false);
    }
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    s_scanPending   = true;
    s_scanStartedMs = millis();
}

static void handleScan() {
    if (!authOk()) return;

    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        sendJson(200, F("{\"state\":\"running\"}"));
        return;
    }
    if (n < 0 || !s_scanPending) {
        // Nothing started yet, a failed scan, or results left over from a
        // scan nobody here asked for: start a fresh one.
        startScan();
        sendJson(200, F("{\"state\":\"running\"}"));
        return;
    }

    String out;
    {
        JsonDocument doc;
        doc["state"] = "done";
        JsonArray nets = doc["nets"].to<JsonArray>();
        // Twenty is more than any hallway shows on a phone, and the JSON is
        // built in RAM on a part that has little of it.
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject o = nets.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["rssi"] = WiFi.RSSI(i);
            o["ch"]   = WiFi.channel(i);
            // The core's own number: ENC_TYPE_NONE (7) is an open network.
            // The page reads it by caps.hw (§6).
            o["enc"]  = WiFi.encryptionType(i);
        }
        serializeJson(doc, out);
    }
    WiFi.scanDelete();
    s_scanPending = false;
    narrowAfterScan();
    sendJson(200, out);
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------

static void handleStatus() {
    if (!authOk()) return;
    const NodeConfig& c = *s_target;
    String out;
    {
        JsonDocument doc;
        doc["uptime_s"] = millis() / 1000UL;
        if (WiFi.status() == WL_CONNECTED) doc["rssi"] = WiFi.RSSI();
        else                               doc["rssi"] = nullptr;
        if (s_link && s_link->everOk)
            doc["last_ok_s"] = (millis() - s_link->lastOkMs) / 1000UL;
        else
            doc["last_ok_s"] = nullptr;
        doc["ip"]    = s_background ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
        doc["mac"]   = WiFi.macAddress();
        doc["rev"]   = c.rev;
        doc["local"] = c.local;
        const char* col = "unknown";
        if (s_link && s_link->attempted) col = s_link->lastOk ? "reachable" : "unreachable";
        doc["collector"] = col;
        // Beyond §6, for whoever reads it by hand: what the sensors said at
        // their last bring-up, and which firmware this is.
        doc["sensors"] = nodeSensorsDescribe();
        doc["fw"]      = (const char*)c.fw;
        serializeJson(doc, out);
    }
    sendJson(200, out);
}

// ---------------------------------------------------------------------------

/// Both portals answer the same routes. Registered once — see s_routesBound
/// above for what re-registering used to cost.
static void bindRoutes() {
    if (s_routesBound) return;
    s_http.on("/", HTTP_GET, handlePage);
    s_http.on("/api/config", HTTP_GET, handleConfigGet);
    s_http.on("/api/config", HTTP_POST, handleConfigPost);
    s_http.on("/api/scan", HTTP_GET, handleScan);
    s_http.on("/api/status", HTTP_GET, handleStatus);
    // Every other path gets the page: that is what makes a phone's captive-
    // portal probe (generate_204, hotspot-detect.html, …) open it.
    s_http.onNotFound(handlePage);
    s_routesBound = true;
}

bool portalRun(NodeConfig& c, uint32_t timeoutMs) {
    s_target     = &c;
    s_saved      = false;
    s_background = false;   // the AP portal: no auth gate

    char ap[32];
    // The chip-id suffix keeps two nodes in one house apart on the air.
    snprintf(ap, sizeof(ap), "esp-node-%04X", (uint16_t)(ESP.getChipId() & 0xFFFF));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap, PORTAL_AP_PASS);
    delay(100);

    const IPAddress ip = WiFi.softAPIP();
    LOGF("[portal] \"%s\" up at http://%s  (pass: %s)\n",
                  ap, ip.toString().c_str(), PORTAL_AP_PASS);
    if (timeoutMs) LOGF("[portal] closing in %lu s\n",
                                 (unsigned long)(timeoutMs / 1000));

    // Answer every DNS query with our own address so phones show the
    // "sign in to network" prompt instead of leaving the user to discover
    // 192.168.4.1 on their own.
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    bindRoutes();
    s_http.begin();

    uint32_t start     = millis();
    bool     hadClient = false;
    while (true) {
        s_dns.processNextRequest();
        s_http.handleClient();
        scanWatchdog();

        if (s_saved) {
            if (millis() - s_savedAt >= kRestartDelayMs) break;
            delay(5);
            continue;
        }

        // THE CLOCK STOPS WHILE SOMEBODY IS CONNECTED, and restarts when they
        // leave. Both halves matter, and an earlier rewrite of this file kept
        // the comment while dropping the assignments — which is worse than
        // never having had them, because the behaviour it describes is the
        // reason the timeout is survivable.
        //
        // The timeout exists so a node does not sit in AP mode forever after a
        // 3 am router reboot nobody witnessed. A station associated to the
        // softAP is the opposite of that: direct evidence a human is standing
        // there, mid-configuration. Without the reset the window closes under
        // them — joining the AP, waiting out the phone's "no internet, stay
        // connected?" prompt and working through the steps is comfortably
        // more than five minutes from boot, and a phone that roams off the
        // no-internet AP for one loop iteration would take the portal with it.
        const bool client = WiFi.softAPgetStationNum() > 0;
        if (client) {
            start = millis();          // hold the window open
            hadClient = true;
        } else if (hadClient) {
            // They left without saving. A fresh window rather than a stale
            // count, so a reconnect gets the whole five minutes again.
            hadClient = false;
            start = millis();
        }

        if (timeoutMs && !client && (millis() - start) > timeoutMs) {
            LOGLN("[portal] timed out, retrying the saved network");
            break;
        }
        delay(5);   // feeds the softAP task; yield() alone starves it here
    }

    s_http.stop();
    s_dns.stop();
    s_scanPending = false;
    s_widenedForScan = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    return s_saved;
}

bool portalStartBackground(NodeConfig& c) {
    // FAILS CLOSED. Without credentials there is nothing between this page and
    // everything on the network, and the page rewrites the node's collector
    // address and restarts it. Refusing is a feature the user has to turn on by
    // setting a password, which is the right way round; serving it anyway would
    // be a remote-configuration interface nobody asked to expose.
    if (c.net.basic_user[0] == '\0' || c.net.basic_pass[0] == '\0') {
        Serial.println(F("[portal] background server NOT started: set a basic-auth "
                         "user and password in the setup page to enable "
                         "configuration over the LAN"));
        return false;
    }

    s_target     = &c;
    s_saved      = false;
    s_background = true;

    bindRoutes();
    s_http.begin();
    LOGF("[portal] background configuration server on http://%s/ "
                  "(password protected)\n", WiFi.localIP().toString().c_str());
    return true;
}

void portalHandleClient() {
    s_http.handleClient();
    if (s_saved) {
        // Let the reply leave before the restart takes the socket with it.
        while (millis() - s_savedAt < kRestartDelayMs) {
            s_http.handleClient();
            delay(5);
        }
        LOGLN("[portal] settings saved via background server, restarting");
        ESP.restart();
    }
}
