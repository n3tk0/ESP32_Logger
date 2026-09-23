// ============================================================================
// ESP32_Logger sensor node — ESP8266 satellite
//
// Reads the sensors its config lists and POSTs the values to an ESP32_Logger
// collector's /api/ingest. The config itself is docs/NODE_CONFIG.md §1: set
// on the node's own page (ConfigPortal.cpp), or by the collector in the reply
// to a POST (§3), which is also how the node follows the collector to a new
// network (§4) and finds it again when its address changes (§3.1).
//
// No storage beyond the config, no display, and — outside the setup page —
// no listening port except the background page on the LAN, and that only
// behind basic auth.
//
// Everything sensor-specific lives in node_common/NodeSensors.cpp (shared with
// the ESP-NOW node); every decision about the config and the link that can be
// made without a board lives in NodeSync.h, where tests/host reaches it.
//
// See README.md in this directory for wiring and setup.
// ============================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <new>

#include "node_config.h"
#include "NodeLog.h"
#include "NodeStore.h"
#include "NodeSync.h"
#include "ConfigPortal.h"
#include "Backlog.h"
#include "node_common/NodeSensors.h"
#include "src/nodecfg/NodeConfigJson.h"
#include "NodeCfgTables.h"
#include "src/nodecfg/UdpDiscovery.h"
#include "src/nodecfg/UdpDiscoveryHmac.h"

using nodecfg::NodeConfig;
using NodeSync::Net;

static NodeConfig        s_cfg;
static SyncState         s_sync;
static NodeSync::Link    s_link;
static PortalLinkStatus  s_linkStatus;

static uint32_t s_lastPost   = 0;
static bool     s_postedOnce = false;

static bool     s_portalBgRunning = false;

/// §3: the full config goes with the first POST after boot (until one
/// carrying it is answered), and with every POST while `local`.
static bool     s_reportedSinceBoot = false;
/// The rev this node last refused — or rolled back. Never re-applied; see
/// NodeSync::decideReply().
static uint16_t s_rejectedRev = 0;
/// A config from the collector changed the network, the I2C pair or the
/// sensors: restart once this cycle's POSTs are done.
static bool     s_restartPending = false;
/// The network settings this boot has been posting with, pinned when a
/// collector config that restarts the node replaces s_cfg mid-cycle. The
/// reply that carried it came over THIS association to THIS host with THIS
/// token; the new settings take effect at the restart (WiFi is only joined at
/// boot), so the batches still to go before it — restartForConfig() hands over
/// the RAM backlog — go where the last one just went. nullptr otherwise; on
/// the heap because it lives only until the restart.
static nodecfg::NetCfg* s_postNet = nullptr;

// ---------------------------------------------------------------------------
// Collector discovery (§3.1)
// ---------------------------------------------------------------------------

/// Broadcast the signed query and adopt whoever answers with a valid reply.
///
/// WHEN: right after this node moved to the collector's next network (§4.5),
/// and after three failed POSTs in a row. Both are "the address this node
/// has for the collector may be wrong"; nothing else is a reason to believe
/// a broadcast over the address the user typed.
///
/// The reply is tagged with the ingest token, so this adopts only a device
/// holding the same token — not whatever on the LAN answers first. The local
/// socket is bound to the discovery port itself, so a collector that replies
/// to the port rather than to the query's source port still reaches it.
static bool runDiscovery() {
    using namespace nodecfg::udpdisc;
    if (WiFi.status() != WL_CONNECTED) return false;

    uint8_t nonce[NONCE_LEN];
    ESP.random(nonce, sizeof(nonce));
    uint8_t query[QUERY_LEN];
    if (!buildQuery(query, nonce, s_cfg.name, s_cfg.net.token, udpdiscHmacSha256)) {
        LOGLN("[disc] cannot build a query (node name?)");
        return false;
    }

    WiFiUDP udp;
    if (!udp.begin(DISCOVERY_PORT)) {
        LOGLN("[disc] cannot open the UDP socket");
        return false;
    }

    bool found = false;
    IPAddress from;
    uint16_t port = 0;
    // Three tries, 700 ms each: a collector busy writing a log file answers
    // late, and a lost broadcast on a busy 2.4 GHz band is common.
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        // The subnet's broadcast AND the limited broadcast: some access points
        // drop one or the other between wireless clients.
        const IPAddress dests[2] = { WiFi.broadcastIP(), IPAddress(255, 255, 255, 255) };
        for (const IPAddress& d : dests) {
            udp.beginPacket(d, DISCOVERY_PORT);
            udp.write(query, sizeof(query));
            udp.endPacket();
        }
        const uint32_t t0 = millis();
        while (!found && millis() - t0 < 700) {
            const int len = udp.parsePacket();
            if (len <= 0) { delay(10); continue; }
            uint8_t buf[REPLY_LEN + 1];
            const int n = udp.read(buf, sizeof(buf));
            uint16_t p = 0;
            if (n > 0 && parseReply(buf, (size_t)n, nonce, s_cfg.net.token,
                                    udpdiscHmacSha256, p)) {
                from  = udp.remoteIP();
                port  = p;
                found = true;
            }
        }
    }
    udp.stop();

    if (!found) {
        LOGLN("[disc] no collector answered");
        return false;
    }

    char host[nodecfg::HOST_CAP];
    nodecfg::copyStr(host, sizeof(host), from.toString().c_str());
    if (strcmp(host, s_cfg.net.host) == 0 && port == s_cfg.net.port) {
        LOGF("[disc] collector confirmed at %s:%u\n", host, (unsigned)port);
        return true;
    }
    LOGF("[disc] collector found at %s:%u (was %s:%u)\n", host, (unsigned)port,
                  s_cfg.net.host, (unsigned)s_cfg.net.port);
    nodecfg::copyStr(s_cfg.net.host, sizeof(s_cfg.net.host), host);
    s_cfg.net.port = port;
    // §3.1: saved and marked local, so the collector learns the address it
    // has on this network from the next report.
    s_cfg.local = true;
    storeSave(s_cfg, !s_link.onTrial());
    return true;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

static bool connectTo(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    for (int attempt = 1; attempt <= 3; attempt++) {
        LOGF("[wifi] connecting to \"%s\" (attempt %d/3)", ssid, attempt);
        WiFi.begin(ssid, pass);

        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
                LOGLN(" timed out");
                break;
            }
            delay(250);
            Serial.print('.');
            yield();
        }
        if (WiFi.status() == WL_CONNECTED) {
            LOGF(" ok, %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        // Clean up before the next attempt.
        WiFi.disconnect();
        delay(1000);
    }
    WiFi.disconnect(true);
    return false;
}

/// Bring WiFi up: three attempts on this cycle's network — the configured
/// one, or the collector's next one once the configured one has failed two
/// cycles running (§4.5) — then the setup portal for PORTAL_TIMEOUT_MS.
static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        // Only flag it as running if it actually started: portalStartBackground()
        // refuses without basic-auth credentials, and setting the flag anyway
        // would have loop() calling portalHandleClient() on a server that was
        // never begun.
        if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
        return true;
    }

    const bool hasNext = s_cfg.net.next.ssid[0] != '\0';
    const Net  which   = s_link.networkToTry(hasNext);
    const bool next    = (which == Net::Next);
    if (next) LOGF("[wifi] \"%s\" failed %u cycles; trying the next network\n",
                            s_cfg.net.ssid, (unsigned)s_link.wifiFails());

    if (connectTo(next ? s_cfg.net.next.ssid : s_cfg.net.ssid,
                  next ? s_cfg.net.next.pass : s_cfg.net.pass)) {
        const uint8_t a = s_link.wifiResult(true, which);
        if (a & NodeSync::LA_PROMOTE) {
            // §4.5: the new network is now this node's network; the old one is
            // kept as next, so a cancelled switch is survivable.
            NodeSync::promoteNext(s_cfg);
            storeSave(s_cfg, !s_link.onTrial());
            LOGF("[wifi] moved to \"%s\" (\"%s\" kept as next)\n",
                          s_cfg.net.ssid, s_cfg.net.next.ssid);
        }
        if (a & NodeSync::LA_DISCOVER) runDiscovery();
        if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
        return true;
    }
    s_link.wifiResult(false, which);

    LOGLN("[wifi] repeated failures — opening setup portal");
    s_portalBgRunning = false; // portalRun will stop the HTTP server on exit
    if (portalRun(s_cfg, PORTAL_TIMEOUT_MS)) {
        LOGLN("[cfg] saved, restarting");
        delay(200);
        ESP.restart();
    }
    // Portal timed out. Return false so the loop can carry on recording.
    return false;
}

// ---------------------------------------------------------------------------
// The backlog
// ---------------------------------------------------------------------------
//
// WHY A NODE THAT CANNOT REACH THE COLLECTOR KEEPS ITS READINGS
//
// It used to read its sensor, fail to POST, print a line and throw the values
// away. A router reboot at 2 am was therefore a hole in the record that
// nothing could fill afterwards: the measurements had existed, briefly, in a
// stack frame. The collector has had a queue for exactly this since the
// ESP-NOW path was written (RemoteIngest::putHistorical) — the HTTP node just
// had nothing to hand it.
//
// A FLAT RING OF READINGS, NOT OF SAMPLES. Grouping by moment would mean
// sizing every slot for NODE_MAX_READINGS whether a node reports three metrics
// or eight, and paying for the eight on the node that reports three. The
// collector treats each reading independently anyway — each one carries its
// own age — so there is nothing for the grouping to buy.
//
// Each entry remembers millis() rather than a date. This node has no clock:
// what it can always say honestly is how long ago, which is what `dt_s` on the
// wire means and what the collector turns back into a timestamp using its own
// NTP-set clock. The same field, meaning the same thing, as EnvSample::dt_s in
// the ESP-NOW protocol.
//
// The ring itself lives in Backlog.h, where a host test can reach it: the
// arithmetic that wraps an index, drops the right end and survives the
// millis() rollover is exactly the part that is wrong quietly. It holds a
// pointer to each metric name; the names live in s_names for the life of the
// process (NodeSync::NameTable says why).

// How many go in one POST. The collector's body cap is 4 KB and a reading is
// roughly sixty bytes of JSON, so this leaves room for long metric names, the
// envelope and the config report rather than discovering the ceiling as a 413.
static constexpr int NODE_BATCH_READINGS = 48;

// The largest body this node will send. Deliberately below the collector's own
// 4096-byte cap rather than equal to it: the two numbers live in different
// firmwares that are not upgraded together, and the margin is what lets an
// older collector keep taking batches from a newer node.
static constexpr size_t NODE_MAX_BODY = 3800;

// How many batches one cycle may send. A node coming back from an outage
// should catch up faster than it accumulates, but not turn a reconnection into
// a burst that the collector's rate limiter reads as a flood.
static constexpr int NODE_BATCHES_PER_CYCLE = 4;

static NodeBacklog::Ring   s_backlog;
static NodeSync::NameTable s_names;

// ---------------------------------------------------------------------------
// The config the collector sends (§3)
// ---------------------------------------------------------------------------

static void rejectConfig(uint16_t rev, const char* field, const char* reason) {
    LOGF("[cfg] refused rev %u: %s: %s\n", (unsigned)rev, field, reason);
    s_rejectedRev = rev;
    s_sync.err.set(rev, field, reason);
    syncSave(s_sync);
}

/// What applying a collector config needs besides the running one: the
/// candidate and the validator's answer, ~1.5 KB, on the heap for the length
/// of the call rather than on the 4 KB loop stack.
struct ApplyWork {
    NodeConfig          cfg;
    nodecfg::Validation v;
    nodecfg::Issue      issue;
};

/// validate → back up → save → apply live, or restart when the network, the
/// I2C pair or the sensor list changed.
static void applyCollectorConfig(JsonVariantConst doc, uint16_t rev) {
    ApplyWork* w = new (std::nothrow) ApplyWork();
    if (!w) {
        LOGLN("[cfg] out of memory for the collector's config; next cycle");
        return;
    }
    w->cfg = s_cfg;
    // NCJ_DEC_REV: this config says which rev it is. Secrets arrive in full
    // here (the one place they travel, §0.5); "" still means keep.
    if (!nodecfg::decodeConfig(doc, w->cfg, nodecfg::NCJ_DEC_REV, &w->issue)) {
        rejectConfig(rev, w->issue.field, w->issue.reason);
        delete w;
        return;
    }
    w->cfg.rev   = rev;
    w->cfg.local = false;   // the collector's config, not an edit on this node

    nodeValidate(w->cfg, w->v);
    if (!w->v.ok) {
        rejectConfig(rev, w->v.error.field, w->v.error.reason);
        delete w;
        return;
    }

    const uint8_t change = NodeSync::classifyChange(s_cfg, w->cfg);
    if (!storeSave(w->cfg, !s_link.onTrial())) {
        // Not refused — the config is fine, the flash is not. Nothing is
        // reported, so the collector keeps offering it and the next cycle
        // tries again.
        LOGLN("[cfg] could not save the collector's config");
        delete w;
        return;
    }
    if (change & NodeSync::CH_NET_TRIAL) {
        // §4.7: this config decides whether the node can reach anything. Run
        // it on trial; the backup taken just now is what a rollback restores.
        s_sync.trialRev = rev;
        syncSave(s_sync);
    }
    // Out of memory leaves it null: the rest of the cycle then posts with the
    // new settings, which is no worse than not pinning at all.
    if ((change & NodeSync::CH_RESTART) && !s_postNet)
        s_postNet = new (std::nothrow) nodecfg::NetCfg(s_cfg.net);
    s_cfg = w->cfg;
    delete w;

    LOGF("[cfg] applied rev %u from the collector%s\n", (unsigned)rev,
                  (change & NodeSync::CH_RESTART) ? "; restarting after this cycle" : "");
    if (change & NodeSync::CH_RESTART) s_restartPending = true;
    // Everything else (name, interval, altitude, board, net.next) is read
    // from s_cfg where it is used, so it is already live.
}

static void handleReplyCfg(JsonVariantConst c) {
    NodeSync::ReplyCfg r;
    r.present = c.is<JsonObjectConst>();
    if (r.present) {
        JsonObjectConst o = c.as<JsonObjectConst>();
        r.rev = o["rev"] | 0;
        bool other = false;
        for (JsonPairConst kv : o) {
            if (strcmp(kv.key().c_str(), "rev") != 0 &&
                strcmp(kv.key().c_str(), "local") != 0) { other = true; break; }
        }
        r.revOnly = !other;
    }

    switch (NodeSync::decideReply(r, s_cfg.rev, s_cfg.local, s_rejectedRev)) {
        case NodeSync::ReplyAction::Confirm:
            // §3: "your local config is now rev N". Same content, so no backup:
            // /config.prev.json keeps the config from before the last change.
            LOGF("[cfg] collector adopted the local config as rev %u\n",
                          (unsigned)r.rev);
            s_cfg.rev   = r.rev;
            s_cfg.local = false;
            storeSave(s_cfg, false);
            break;
        case NodeSync::ReplyAction::Apply:
            applyCollectorConfig(c, r.rev);
            break;
        case NodeSync::ReplyAction::Ignore:
        case NodeSync::ReplyAction::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// Post
// ---------------------------------------------------------------------------

enum class PostResult : uint8_t {
    Delivered,   ///< 200, and the collector took the whole batch
    Held,        ///< 200, but it took less than all of it: its queue is full
    Failed,      ///< no answer, or not a 200
};

/// Send up to NODE_BATCH_READINGS from the front of the backlog — or, with
/// nothing queued, none: a POST with an empty `readings` still reports the
/// config and still collects the reply's, so a node whose every sensor is
/// missing can be fixed from the collector.
///
/// Delivered only when the collector took the WHOLE batch — which is not the
/// same as "the POST returned 200", and the difference is the point. The
/// collector answers with how many readings from the front of the batch it
/// consumed; whatever it had no room for is still here and is offered again
/// next cycle. Dropping the batch on the strength of the status line is how an
/// outage at the collector becomes a hole in the record on the node.
static PostResult postBatch() {
    const int want = (s_backlog.count() < NODE_BATCH_READINGS) ? s_backlog.count()
                                                               : NODE_BATCH_READINGS;
    const uint32_t nowMs = millis();

    const bool withCfg = NodeSync::shouldSendCfg(s_cfg.local, s_reportedSinceBoot);
    const uint16_t errRev = s_sync.err.rev;

    String body;
    int n = 0;
    {
        JsonDocument doc;
        doc["node"] = (const char*)s_cfg.name;
        // Still 0: this node has no clock, so the collector stamps the batch
        // with its own and subtracts each reading's age from it.
        doc["ts"] = 0;

        // §3. cfg_rev always; the config itself without secrets; a refusal
        // until it has been delivered once.
        doc["cfg_rev"] = s_cfg.rev;
        if (withCfg) nodecfg::encodeConfig(s_cfg, doc["cfg"].to<JsonObject>(), 0);
        if (s_sync.err.pending()) {
            JsonObject e = doc["cfg_error"].to<JsonObject>();
            e["rev"]    = s_sync.err.rev;
            e["field"]  = (const char*)s_sync.err.field;
            e["reason"] = (const char*)s_sync.err.reason;
        }

        JsonArray readings = doc["readings"].to<JsonArray>();
        for (int i = 0; i < want; i++) {
            const NodeBacklog::Entry& p = s_backlog.at(i);
            JsonObject o = readings.add<JsonObject>();
            o["metric"] = p.metric;
            o["value"]  = p.value;
            o["unit"]   = p.unit;
            const uint32_t age = NodeBacklog::ageSeconds(nowMs, p.ms);
            if (age > 0) o["dt_s"] = age;
        }

        // TRIMMED TO FIT, BECAUSE A 413 HERE WOULD DEADLOCK THE QUEUE.
        //
        // The collector refuses a body over INGEST_MAX_BODY (4 KB) with a 413,
        // and 413 holds the batch — correctly, since a refused POST must not
        // cost readings. But a batch that is too big is refused again next
        // cycle and every cycle after: the queue never empties, and the node
        // quietly stops recording anything new the moment it fills. Forty-
        // eight readings reach ~3.6 KB only with the longest names, and the
        // config report adds up to ~1 KB on the first POST after boot — so
        // this loop does run then, and costs that one POST a few readings,
        // which the next one carries.
        while (readings.size() > 1 && measureJson(doc) > NODE_MAX_BODY) {
            readings.remove(readings.size() - 1);
        }
        n = (int)readings.size();
        if (doc.overflowed()) {
            LOGLN("[post] out of memory building the batch");
            return PostResult::Failed;
        }
        serializeJson(doc, body);
    }

    const nodecfg::NetCfg& net = s_postNet ? *s_postNet : s_cfg.net;
    WiFiClient  client;
    HTTPClient  http;
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u/api/ingest",
             net.host, (unsigned)net.port);

    s_linkStatus.attempted = true;
    if (!http.begin(client, url)) {
        LOGLN("[post] http.begin failed");
        s_linkStatus.lastOk = false;
        return PostResult::Failed;
    }
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ingest-Token", net.token);
    if (net.basic_user[0] != '\0') {
        http.setAuthorization(net.basic_user, net.basic_pass);
    }

    const int code = http.POST(body);
    body = String();   // the request is gone; give its ~3.8 KB back before the reply
    const String reply = (code > 0) ? http.getString() : String();
    http.end();

    if (code != 200) {
        // No answer, or one that is not this endpoint's. Keep everything: this
        // is the case the queue exists for.
        if (code > 0) LOGF("[post] %d (%d held)\n", code, s_backlog.count());
        else LOGF("[post] %s (%d held)\n",
                           http.errorToString(code).c_str(), s_backlog.count());
        s_linkStatus.lastOk = false;
        const uint8_t a = s_link.postResult(false);
        // Not while a restart is pending: discovery rewrites s_cfg's host,
        // which is the collector's new config now, not the one posting.
        if ((a & NodeSync::LA_DISCOVER) && !s_restartPending) runDiscovery();
        return PostResult::Failed;
    }

    s_linkStatus.lastOk   = true;
    s_linkStatus.everOk   = true;
    s_linkStatus.lastOkMs = millis();
    if (s_link.postResult(true) & NodeSync::LA_TRIAL_OK) {
        LOGLN("[cfg] the new network settings reached the collector; kept");
        s_sync.trialRev = 0;
        syncSave(s_sync);
    }
    // Delivered with this POST: the report and the refusal.
    if (withCfg) s_reportedSinceBoot = true;
    if (errRev && s_sync.err.rev == errRev) {
        s_sync.err.clear();
        syncSave(s_sync);
    }

    // ── What the collector says it did, and what this node does about it ────
    //
    // `accepted` is the length of the PREFIX of the batch the collector
    // consumed. It is not the same as "how many it liked": a reading it threw
    // out as unusable is still consumed, because re-sending it would fail the
    // same way for ever. What is NOT consumed is whatever it had no room for —
    // its history queue is 64 readings and this node offers 192 — and those
    // are still here, in order, to offer again when it has drained some.
    //
    // A collector that predates this field answers without it. Defaulting to
    // `n` is what makes that upgrade path work: an older collector took the
    // batch through the live mailbox and re-sending would only overwrite it,
    // so the whole batch counts as delivered.
    //
    // THE REPLY CAN CARRY A WHOLE CONFIG (§3), secrets included — up to ~2 KB
    // of JSON. ArduinoJson 7's document grows as it parses, so there is no
    // fixed buffer to outgrow; what bounds it is the heap, and the request
    // body has already been released above to make room.
    int accepted = n;
    int room     = -1;
    {
        JsonDocument res;
        if (deserializeJson(res, reply) == DeserializationError::Ok) {
            accepted = res["accepted"] | n;
            room     = res["room"]     | -1;
            handleReplyCfg(res["cfg"]);
        } else {
            // 200 with a body this node cannot read: something in the way
            // answering for the collector, or an out-of-memory parse. The
            // status line is all there is to go on, and a node that never
            // empties its queue stops being able to record anything new.
            LOGF("[post] %d readings -> 200, unreadable reply\n", n);
        }
    }

    if (n == 0) return PostResult::Delivered;   // a report with nothing queued

    if (accepted < 0) accepted = 0;
    if (accepted > n) accepted = n;

    if (accepted == 0) {
        // Up, reachable, and taking nothing: its queue is full and needs a
        // sensor tick or two to drain. Everything stays here.
        //
        // THERE IS NO ATTEMPT LIMIT AND NO GIVING UP, deliberately. If the
        // collector never drains — no remote sensor configured for this node,
        // say — the ring shedding its own oldest entry is already the right
        // answer, and it goes on recording throughout. A node that threw a
        // batch away to unstick itself would be discarding readings to solve a
        // problem at the other end.
        LOGF("[post] collector took 0 of %d (room %d, %d held)\n",
                      n, room, s_backlog.count());
        return PostResult::Held;
    }

    s_backlog.drop(accepted);

    if (accepted < n) {
        LOGF("[post] %d of %d taken (room %d, %d held)\n",
                      accepted, n, room, s_backlog.count());
        return PostResult::Held;    // it is full; the next batch would only be refused
    }

    LOGF("[post] %d readings taken (%d still held)\n",
                  accepted, s_backlog.count());
    return PostResult::Delivered;
}

/// Read the sensors and remember what they said. NEEDS NO NETWORK, and that
/// is the point: the cycle where the link is down is the cycle whose readings
/// the queue exists to keep.
static void collectReading() {
    NodeReading vals[NODE_MAX_READINGS];
    const int n = nodeSensorsRead(s_cfg, vals, NODE_MAX_READINGS);
    if (n == 0) {
        LOGLN("[sensor] nothing to record this cycle");
        return;
    }
    const uint32_t ms = millis();
    for (int i = 0; i < n; i++) {
        const char* name = s_names.intern(vals[i].name);
        if (!name) {
            LOGF("[sensor] no room to name \"%s\"; dropped\n", vals[i].name);
            continue;
        }
        s_backlog.push(NodeBacklog::Entry{ms, vals[i].value, name, vals[i].unit});
    }
}

/// Hand over as much of the backlog as the collector will take. Returns true
/// when any POST this cycle was answered — the collector is there.
///
/// THE NEW READING GOES THROUGH THE SAME QUEUE as everything else rather than
/// down a separate live path. One route means one set of rules: it is sent
/// with an age of zero when the link is up, and it is still there — with an
/// honest age — when the link was down.
static bool flushBacklog(int maxBatches) {
    // Nothing queued still gets one POST: the config exchange rides on it.
    if (s_backlog.count() == 0) return postBatch() != PostResult::Failed;

    // Several batches, so a node coming back from an outage catches up faster
    // than it accumulates — but a bounded number, so a reconnection is not a
    // burst the collector's rate limiter reads as a flood. Stops at the first
    // batch that does not leave the queue: whatever refused one will refuse
    // the next, and the point is to wait, not to hammer.
    bool answered = false;
    for (int b = 0; b < maxBatches && s_backlog.count() > 0; b++) {
        const PostResult r = postBatch();
        if (r != PostResult::Failed) answered = true;
        if (r != PostResult::Delivered) break;
        yield();
    }
    return answered;
}

/// §4.7: the network settings the collector sent reached nothing for five
/// cycles. Put the previous config back, tell the collector why on the first
/// POST that gets through, and restart on the old settings.
static void rollBack() {
    const uint16_t rev = s_sync.trialRev;
    s_sync.trialRev = 0;
    if (!storeRollback()) {
        LOGLN("[cfg] no collector on the new settings, and no backup to "
                       "return to; keeping them");
        syncSave(s_sync);
        return;
    }
    s_sync.err.set(rev, NodeSync::ROLLBACK_FIELD, NodeSync::ROLLBACK_REASON);
    syncSave(s_sync);
    LOGF("[cfg] rev %u rolled back: no collector on the new settings; restarting\n",
                  (unsigned)rev);
    delay(200);
    ESP.restart();
}

static void restartForConfig() {
    // The backlog is RAM and does not survive the restart, so hand over what
    // the collector will take first — it has just answered, so it is there.
    // Bounded: a collector with a full queue takes nothing, and the new config
    // is not worth waiting indefinitely for.
    if (s_backlog.count() > 0) flushBacklog(8);
    if (s_backlog.count() > 0)
        LOGF("[cfg] restarting with %d readings not delivered\n", s_backlog.count());
    LOGLN("[cfg] restarting to apply the collector's config");
    delay(200);
    ESP.restart();
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    LOGLN("\n\nESP32_Logger sensor node " NODE_FW_VERSION);

    // Read the button before anything else claims GPIO0.
    const bool forcePortal = portalButtonHeld();

    storeLoad(s_cfg);
    syncLoad(s_sync);
    if (s_sync.trialRev) {
        LOGF("[cfg] rev %u's network settings are on trial\n",
                      (unsigned)s_sync.trialRev);
        s_link.beginTrial();
    }
    // A refusal (or rollback) not yet reported is also one not to take again
    // if the collector offers it before it has heard.
    if (s_sync.err.pending()) s_rejectedRev = s_sync.err.rev;
    portalSetLinkStatus(&s_linkStatus);

    {
        // Said at boot, not enforced: a config migrated from the old format
        // runs even if today's rules would refuse it (a node name with a dot
        // in it, say), because refusing would orphan its history on the
        // collector. The page shows the same complaint when it is saved.
        nodecfg::Validation* v = new (std::nothrow) nodecfg::Validation();
        if (v) {
            nodeValidate(s_cfg, *v);
            if (!v->ok) LOGF("[cfg] warning: %s: %s\n", v->error.field, v->error.reason);
            delete v;
        }
    }

    // Two reasons to run the portal with no timeout: there is nothing to fall
    // back to, or the user explicitly asked by holding FLASH through reset.
    // Both mean "wait for a human", so waiting indefinitely is correct.
    if (forcePortal || !storeIsComplete(s_cfg)) {
        if (forcePortal) LOGLN("[portal] FLASH held at boot");
        else             LOGLN("[portal] no usable config");
        if (portalRun(s_cfg, 0)) {
            delay(200);
            ESP.restart();
        }
    }

    LOGF("node \"%s\" -> %s:%u every %u s (rev %u%s)\n",
                  s_cfg.name, s_cfg.net.host, (unsigned)s_cfg.net.port,
                  (unsigned)s_cfg.interval_s, (unsigned)s_cfg.rev,
                  s_cfg.local ? ", local" : "");

    nodeSensorsBegin(s_cfg);
    LOGF("sensors: %s\n", nodeSensorsDescribe());
    ensureWifi();
}

void loop() {
    if (s_portalBgRunning) {
        portalHandleClient();
    }

    const uint32_t now = millis();

    // Unsigned subtraction, so the ~49-day millis() wrap is a non-event.
    if (s_postedOnce && (now - s_lastPost) < (uint32_t)s_cfg.interval_s * 1000UL) {
        delay(50);
        return;
    }
    s_lastPost   = now;
    s_postedOnce = true;

    // THE SENSOR PROBE FIRST, THEN THE NETWORK, AND NEITHER GATES THE OTHER.
    //
    // Both orderings have been wrong here. With the WiFi check first, a node
    // that cannot reach its router never re-probes its sensor — ensureWifi()
    // spends three connect timeouts and can block in the portal, then returns
    // false, every cycle forever, and a power cut that took out both the
    // router and a cold breakout leaves the sensor unfound until someone
    // walks to it. With the sensor check first (as it once shipped), a node
    // whose sensor was missing returned before ever reaching ensureWifi(), so
    // it never reconnected after a router reboot — and the background page,
    // the one way to fix the wiring without a cable, is started from in there.
    //
    // The probe needs no network and costs a few milliseconds, so it goes
    // first and unconditionally; the network follows and is likewise not
    // conditional on the sensor.
    if (!nodeSensorsReady()) nodeSensorsBegin(s_cfg);

    // MEASURED AND REMEMBERED BEFORE THE NETWORK IS EVEN LOOKED AT. The one
    // thing the backlog exists for — the cycle where the router is down — must
    // not be the one cycle whose reading was never taken.
    if (nodeSensorsReady()) collectReading();

    if (ensureWifi()) flushBacklog(NODE_BATCHES_PER_CYCLE);

    if (s_link.cycleEnd() & NodeSync::LA_ROLLBACK) rollBack();
    if (s_restartPending) restartForConfig();
}
