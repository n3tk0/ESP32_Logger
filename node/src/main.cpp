// ============================================================================
// ESP32_Logger sensor node — ESP8266 satellite
//
// Reads whatever sensors this build selected (see node_config.h) and POSTs
// the values to an ESP32_Logger collector's /api/ingest. That is the whole
// job: no storage, no display, and — outside the setup portal — no server and
// no listening port.
//
// Everything sensor-specific lives in sensors.cpp, so this file has no #ifdef
// per driver. Metric names and units match the collector's own plugins
// exactly, so a remote reading and a wired one are the same series shape.
//
// See README.md in this directory for wiring and setup.
// ============================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include "node_config.h"
#include "NodeSettings.h"
#include "ConfigPortal.h"
#include "sensors.h"
#include "Backlog.h"

static NodeSettings s_cfg;
static uint32_t     s_lastPost   = 0;
static bool         s_postedOnce = false;

static bool         s_portalBgRunning = false;


// Bring WiFi up, retrying 3 times back-to-back before offering the setup portal.
static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        // Only flag it as running if it actually started: portalStartBackground()
        // refuses without basic-auth credentials, and setting the flag anyway
        // would have loop() calling portalHandleClient() on a server that was
        // never begun.
        if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);

    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[wifi] connecting to \"%s\" (attempt %d/3)", s_cfg.ssid, attempt);
        WiFi.begin(s_cfg.ssid, s_cfg.pass);

        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
                Serial.println(" timed out");
                break; // break the while loop to retry
            }
            delay(250);
            Serial.print('.');
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf(" ok, %s\n", WiFi.localIP().toString().c_str());
            if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
            return true;
        }

        // Clean up before next attempt
        WiFi.disconnect();
        delay(1000);
    }

    WiFi.disconnect(true);
    Serial.println("[wifi] repeated failures — opening setup portal");
    s_portalBgRunning = false; // portalRun will stop the HTTP server on exit
    if (portalRun(s_cfg, PORTAL_TIMEOUT_MS)) {
        Serial.println("[cfg] saved, restarting");
        delay(200);
        ESP.restart();
    }
    
    // Portal timed out. Return false so loop can sleep.
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
// or twelve, and paying for the twelve on the node that reports three. The
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
// millis() rollover is exactly the part that is wrong quietly.

// How many go in one POST. The collector's body cap is 4 KB and a reading is
// roughly sixty bytes of JSON, so this leaves room for long metric names and
// the envelope rather than discovering the ceiling as a 413.
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

static NodeBacklog::Ring s_backlog;

// ---------------------------------------------------------------------------
// Post
// ---------------------------------------------------------------------------

/// Send up to NODE_BATCH_READINGS from the front of the backlog.
///
/// Returns true only when the collector took the WHOLE batch — which is not
/// the same as "the POST returned 200", and the difference is the point. The
/// collector answers with how many readings from the front of the batch it
/// consumed; whatever it had no room for is still here and is offered again
/// next cycle. Dropping the batch on the strength of the status line is how an
/// outage at the collector becomes a hole in the record on the node.
///
/// False therefore means one of three things — no answer, a bad answer, or a
/// full queue — and all three ask for the same thing: stop sending for now.
static bool postBatch() {
    if (s_backlog.count() == 0) return false;

    const int want = (s_backlog.count() < NODE_BATCH_READINGS) ? s_backlog.count()
                                                               : NODE_BATCH_READINGS;
    const uint32_t nowMs = millis();

    JsonDocument doc;
    doc["node"] = s_cfg.nodeId;
    // Still 0: this node has no clock, so the collector stamps the batch with
    // its own and subtracts each reading's age from it.
    doc["ts"] = 0;

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
    // The collector refuses a body over INGEST_MAX_BODY (4 KB) with a 413, and
    // 413 is one of the codes below that holds the batch — correctly, since a
    // refused POST must not cost readings. But a batch that is too big is
    // refused again next cycle and every cycle after: the queue never empties,
    // and the node quietly stops recording anything new the moment it fills.
    // Forty-eight readings only reach ~3.6 KB with the longest metric and unit
    // names this node has, so the loop below almost never runs — which is the
    // point. It is here for the build with the long names that does not exist
    // yet, where the alternative is a node that looks connected and is not.
    while (readings.size() > 1 && measureJson(doc) > NODE_MAX_BODY) {
        readings.remove(readings.size() - 1);
    }
    const int n = (int)readings.size();

    String body;
    serializeJson(doc, body);

    WiFiClient  client;
    HTTPClient  http;
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u/api/ingest",
             s_cfg.host, (unsigned)s_cfg.port);

    if (!http.begin(client, url)) {
        Serial.println("[post] http.begin failed");
        return false;
    }
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ingest-Token", s_cfg.token);
    if (s_cfg.basicUser[0] != '\0') {
        http.setAuthorization(s_cfg.basicUser, s_cfg.basicPass);
    }

    const int    code  = http.POST(body);
    const String reply = (code > 0) ? http.getString() : String();
    http.end();

    if (code != 200) {
        // No answer, or one that is not this endpoint's. Keep everything: this
        // is the case the queue exists for.
        if (code > 0) Serial.printf("[post] %d (%d held)\n", code, s_backlog.count());
        else Serial.printf("[post] %s (%d held)\n",
                           http.errorToString(code).c_str(), s_backlog.count());
        return false;
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
    JsonDocument res;
    int accepted = n;
    int room     = -1;
    if (deserializeJson(res, reply) == DeserializationError::Ok) {
        accepted = res["accepted"] | n;
        room     = res["room"]     | -1;
    } else {
        // 200 with a body this node cannot read: something in the way
        // answering for the collector, or an out-of-memory parse. The status
        // line is all there is to go on, and a node that never empties its
        // queue stops being able to record anything new.
        Serial.printf("[post] %d readings -> 200, unreadable reply\n", n);
    }

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
        Serial.printf("[post] collector took 0 of %d (room %d, %d held)\n",
                      n, room, s_backlog.count());
        return false;
    }

    s_backlog.drop(accepted);

    if (accepted < n) {
        Serial.printf("[post] %d of %d taken (room %d, %d held)\n",
                      accepted, n, room, s_backlog.count());
        return false;    // it is full; the next batch would only be refused
    }

    Serial.printf("[post] %d readings taken (%d still held)\n",
                  accepted, s_backlog.count());
    return true;
}

/// Read the sensor and remember what it said. NEEDS NO NETWORK, and that is
/// the point: the cycle where the link is down is the cycle whose readings the
/// queue exists to keep.
static void collectReading() {
    NodeReading vals[NODE_MAX_READINGS];
    const int n = sensorsRead(s_cfg, vals, NODE_MAX_READINGS);
    if (n == 0) {
        Serial.println("[sensor] nothing to record this cycle");
        return;
    }
    const uint32_t ms = millis();
    for (int i = 0; i < n; i++) {
        s_backlog.push(NodeBacklog::Entry{ms, vals[i].value,
                                          vals[i].metric, vals[i].unit});
    }
}

/// Hand over as much of the backlog as the collector will take.
///
/// THE NEW READING GOES THROUGH THE SAME QUEUE as everything else rather than
/// down a separate live path. One route means one set of rules: it is sent
/// with an age of zero when the link is up, and it is still there — with an
/// honest age — when the link was down. A second path for the live case is a
/// second place for the queue to be got wrong.
static bool flushBacklog() {
    if (s_backlog.count() == 0) return false;

    // Several batches, so a node coming back from an outage catches up faster
    // than it accumulates — but a bounded number, so a reconnection is not a
    // burst the collector's rate limiter reads as a flood. Stops at the first
    // batch that does not leave the queue: whatever refused one will refuse
    // the next, and the point is to wait, not to hammer.
    bool sentAny = false;
    for (int b = 0; b < NODE_BATCHES_PER_CYCLE && s_backlog.count() > 0; b++) {
        if (!postBatch()) break;
        sentAny = true;
        yield();
    }
    return sentAny;
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\nESP32_Logger sensor node");

    // Read the button before anything else claims GPIO0.
    const bool forcePortal = portalButtonHeld();

    settingsLoad(s_cfg);

    // Two reasons to run the portal with no timeout: there is nothing to fall
    // back to, or the user explicitly asked by holding FLASH through reset.
    // Both mean "wait for a human", so waiting indefinitely is correct.
    if (forcePortal || !s_cfg.isComplete()) {
        Serial.println(forcePortal ? "[portal] FLASH held at boot"
                                   : "[portal] no usable config");
        if (portalRun(s_cfg, 0)) {
            delay(200);
            ESP.restart();
        }
    }

    Serial.printf("node \"%s\" -> %s:%u every %lu s\n",
                  s_cfg.nodeId, s_cfg.host, (unsigned)s_cfg.port,
                  (unsigned long)(s_cfg.intervalMs / 1000UL));

    sensorsBegin(s_cfg);
    Serial.printf("sensors: %s\n", sensorsDescribe());
    ensureWifi();
}

void loop() {
    if (s_portalBgRunning) {
        portalHandleClient();
    }

    const uint32_t now = millis();

    // Unsigned subtraction, so the ~49-day millis() wrap is a non-event.
    if (s_postedOnce && (now - s_lastPost) < s_cfg.intervalMs) {
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
    // walks to it. With the sensor check first (as it shipped), a node whose
    // sensor was missing returned before ever reaching ensureWifi(), so it
    // never reconnected after a router reboot — and portalStartBackground(),
    // the one way to fix the wiring without a cable, is called from in there.
    //
    // The probe needs no network and costs a few milliseconds, so it goes
    // first and unconditionally; the network follows and is likewise not
    // conditional on the sensor. Only the POST needs both.
    if (!sensorsReady()) sensorsBegin(s_cfg);

    // MEASURED AND REMEMBERED BEFORE THE NETWORK IS EVEN LOOKED AT. This used
    // to read the sensor after ensureWifi(), which meant the one thing the
    // backlog exists for — the cycle where the router is down — was the one
    // cycle whose reading was never taken. ensureWifi() can also spend three
    // connect timeouts and open the portal before returning, so the value
    // recorded here is closer to the moment it belongs to as well.
    if (sensorsReady()) collectReading();

    if (!ensureWifi()) return;   // the queue keeps; it is emptied next cycle

    flushBacklog();
}
