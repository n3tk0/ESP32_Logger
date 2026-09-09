#include "IngestHandler.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <math.h>                     // isfinite(), before the ingest calls do it
#include <time.h>                     // the collector's clock, to judge the node's

#include "IngestBatch.h"
#include "RateLimiter.h"
#include "../sensors/RemoteIngest.h"

#ifndef INGEST_TOKEN
#  define INGEST_TOKEN "change-me"
#endif

// A node payload is a handful of small objects. Anything larger is either a
// misconfigured client or someone probing, and buffering it would be the
// only unbounded allocation on this path.
//
// FOUR KILOBYTES AND NOT ONE, because a node handing over a buffered outage
// sends more than one moment at a time. At roughly 60 bytes of JSON per
// reading, 1 KB was about sixteen readings — five samples of a three-metric
// node — so a backlog had to be dribbled out five samples per POST, and a
// node reporting every minute could not catch up on an outage faster than it
// was accumulating a new one. It is still a fixed ceiling and still the only
// buffer on this path; it is just one that fits the job the path now has.
static constexpr size_t INGEST_MAX_BODY = 4096;

// Length-independent compare. The token is short and this endpoint is rate
// limited, so a timing oracle here is largely theoretical — but the whole
// comparison is four lines, and "we only got it right where it was hard" is
// how the easy cases end up wrong.
static bool tokenMatches(const char* got) {
    const char* want = INGEST_TOKEN;
    if (got == nullptr) return false;

    const size_t wantLen = strlen(want);
    const size_t gotLen  = strlen(got);

    // A length mismatch alone decides the result, but the loop still runs
    // over the common prefix so a correct-length guess gets no earlier exit
    // than a wrong one. The length itself is not secret.
    uint8_t diff = (uint8_t)(wantLen != gotLen);
    const size_t n = (wantLen < gotLen) ? wantLen : gotLen;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(want[i] ^ got[i]);

    return diff == 0;
}

static bool authorised(AsyncWebServerRequest* req) {
    if (req->hasHeader("X-Ingest-Token")) {
        return tokenMatches(req->getHeader("X-Ingest-Token")->value().c_str());
    }
    if (req->hasParam("token")) {
        return tokenMatches(req->getParam("token")->value().c_str());
    }
    return false;
}

static void handleIngestBody(AsyncWebServerRequest* req, uint8_t* data,
                             size_t len, size_t index, size_t total) {
    // Shape checks FIRST, and they answer only on the opening segment.
    //
    // ESPAsyncWebServer calls this once per segment of a chunked body. Every
    // req->send() overwrites the request's response object — leaking the
    // previous one — and writes another HTTP response onto the same socket,
    // so replying per segment corrupts the connection. Answering on index 0
    // and staying silent afterwards is what keeps that to one response.
    //
    // The size check has to come before the single-chunk check too: a body
    // larger than the cap is exactly what arrives split, so testing it second
    // made its own error message unreachable.
    if (total > INGEST_MAX_BODY) {
        if (index == 0) {
            req->send(413, "application/json",
                      "{\"ok\":false,\"error\":\"body too large\"}");
        }
        return;
    }
    if (index != 0 || len != total) {
        if (index == 0) {
            req->send(413, "application/json",
                      "{\"ok\":false,\"error\":\"body must arrive in one chunk\"}");
        }
        return;
    }

    // Auth before the rate limiter: the bucket is device-wide, so checking it
    // first let an unauthenticated caller drain it and lock out the real node.
    if (!authorised(req)) {
        req->send(401, "application/json",
                  "{\"ok\":false,\"error\":\"bad or missing ingest token\"}");
        return;
    }
    if (rateLimit429(req)) return;

    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }

    const char* node = body["node"] | "";
    if (*node == '\0') {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"missing node id\"}");
        return;
    }

    JsonArrayConst readings = body["readings"];
    if (readings.isNull()) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"missing readings array\"}");
        return;
    }

    // ── The node's timestamp, and how far it is trusted ─────────────────────
    //
    // This used to be decorative: SensorManager stamped the collector's clock
    // over every reading regardless, so whatever a node sent here was
    // discarded. That changed when remote readings were allowed to keep the
    // time they were measured — the field is authoritative now, and an
    // authoritative field arriving over the network from a device with no RTC
    // has to be checked rather than believed.
    //
    // The rule: this endpoint takes a batch of readings sampled NOW. There is
    // one `ts` for the whole batch and no way to mark it as backfill, so a
    // stamp that the collector's own pipeline would classify as history is a
    // stamp that is wrong — the same ±120 s the backfill test uses, so that
    // /api/ingest cannot produce a reading its own pipeline then hides.
    //
    // An implausible stamp costs the STAMP, not the reading. Falling back to
    // zero hands the job to SensorManager, which dates it on arrival: a
    // reading a few seconds late in the record beats one filed under the wrong
    // hour, and beats one dropped. The count comes back in the response so a
    // node with a drifting clock can find out it has one.
    //
    // A collector with no clock of its own cannot judge, and there the node's
    // epoch is better than nothing: it is taken as sent.
    uint32_t ts = body["ts"] | 0UL;
    const uint32_t nowEpoch = (uint32_t)time(nullptr);
    bool clockRejected = false;

    if (ts != 0 && nowEpoch >= 1000000000u) {
        const bool tooOld    = ts < 1000000000u ||
                               (ts <= nowEpoch && (nowEpoch - ts) > 120u);
        const bool tooFuture = ts > nowEpoch && (ts - nowEpoch) > 120u;
        if (tooOld || tooFuture) {
            ts = 0;
            clockRejected = true;
        }
    }

    // ── Readings the node measured earlier and is only now handing over ─────
    //
    // `dt_s` on a reading is how many seconds BEFORE this batch it was taken.
    // It is the same field, meaning the same thing, as EnvSample::dt_s in the
    // ESP-NOW protocol — a node with no clock cannot send an absolute time,
    // but it can always say how long ago.
    //
    // Those go to putHistorical(), not put(). put() is a mailbox slot and
    // overwrites: right for a node reporting faster than the collector ticks,
    // wrong for fifteen distinct measurements from an outage, which it would
    // collapse into one. RemoteIngest has had the queue for this since the
    // ESP-NOW path was written; this endpoint simply never used it, so an
    // HTTP node had nowhere to put a backlog and dropped it instead.
    //
    // THE BASE IS THE COLLECTOR'S CLOCK unless the node's own stamp survived
    // the check above. Anchoring an age to a clock that is not set produces a
    // 1970 date, which storage would keep and the chart would file under an
    // hour that has not happened — so with no clock, an age is discarded and
    // the reading is taken as live. A gap is better than a wrong date, and it
    // is the same judgement putHistorical() makes for itself.
    const uint32_t base        = (ts != 0) ? ts : nowEpoch;
    const bool     canBackfill = (nowEpoch >= 1000000000u);

    // ── Which readings are the node's CURRENT value, and which are history ──
    //
    // The rule, and why it is wrong in both directions, is in IngestBatch.h
    // where a host test can reach it. Sixteen names is past what a node can
    // report (the reference node's NODE_MAX_READINGS is 12).
    static constexpr int MAX_LIVE = 16;
    int liveIdx[MAX_LIVE];
    const int nLive = IngestBatch::findNewestPerMetric(
        (int)readings.size(),
        [&readings](int i) -> const char* { return readings[i]["metric"] | ""; },
        liveIdx, MAX_LIVE);

    // ── What the counters mean, and why the batch can stop early ───────────
    //
    // `accepted` is the length of the PREFIX of this batch that the collector
    // consumed — stored, queued, or judged unusable. It is the only number the
    // node needs: it drops exactly that many from the front of its own buffer
    // and keeps the rest, in order, to offer again.
    //
    // WHY A PREFIX AND NOT THE WHOLE BATCH. The history queue holds 64
    // readings and drains a handful per sensor tick; an ESP8266 handing over
    // an hour-long outage offers 192. putHistorical() never refuses for want
    // of room — it sheds the oldest and takes the new one, which is the only
    // thing it can do for ESP-NOW, where the node is already asleep by the
    // time the frame is parsed. Letting it do that here would have shredded
    // two thirds of that outage inside the collector, seconds after the node
    // had gone to the trouble of keeping it. The node is still on the line and
    // has three times the room, so it is told to wait instead: this loop stops
    // at the first backfill reading there is no space for, and everything from
    // there on stays where it already is.
    //
    // A reading the collector cannot use — no metric name, a value that is not
    // a number, a date before 2001 — is consumed rather than blocking the
    // queue behind it. Sending it again would fail the same way for ever.
    int stored = 0, queued = 0, rejected = 0, accepted = 0;
    bool backpressure = false;
    int  idx = -1;

    for (JsonObjectConst r : readings) {
        idx++;
        const char* metric = r["metric"] | "";
        const char* unit   = r["unit"]   | "";
        // No default: an absent value must be rejected, not read as 0.
        const bool  hasValue = r["value"].is<float>();
        const float value    = hasValue ? r["value"].as<float>() : 0.0f;

        const uint32_t age    = IngestBatch::clampAge(r["dt_s"] | 0UL);
        const bool     newest = IngestBatch::isNewest(liveIdx, nLive, idx);
        const bool     backfill =
            IngestBatch::isBackfill(newest, age, canBackfill, base);
        const uint32_t when = backfill ? (base - age) : 0;

        // ROOM CHECKED BEFORE THE READING IS JUDGED, so that a batch stops at
        // the same place whatever it happens to contain. Deciding to stop is
        // about the queue, not about this reading.
        if (backfill && remoteIngest.historyRoom() <= 0) {
            backpressure = true;
            break;
        }

        accepted++;

        // What both put() and putHistorical() refuse outright, tested here so
        // that a false from either below has exactly one meaning left.
        if (!hasValue || *metric == '\0' || !isfinite(value) ||
            (backfill && when < 1000000000u)) {
            rejected++;
            continue;
        }

        if (backfill) {
            // There is room — checked above — so this cannot shed anything,
            // and its return value cannot mean anything but success.
            remoteIngest.putHistorical(node, metric, value, unit, when);
            queued++;
        } else if (remoteIngest.put(node, metric, value, unit,
                                    (canBackfill && base > age) ? base - age : ts)) {
            // The mailbox keeps the time the reading was TAKEN, not the time
            // it arrived: a node that spent four seconds reconnecting before
            // posting measured four seconds ago, and put() honours a stamp
            // rather than overwriting it. Falls back to the batch stamp — 0
            // included, which is SensorManager's cue to date it on arrival —
            // when there is no clock to anchor an age to.
            stored++;
        } else {
            // The mailbox is full of other nodes' metrics. Consumed anyway:
            // holding the reading would not make a slot appear, and the node
            // would stop recording new ones behind it.
            rejected++;
        }
    }

    // `stored` and `queued` are reported apart because they answer different
    // questions — how much is current, how much was backlog — and `held` says
    // plainly that the rest of the batch was not read at all, so a node that
    // gets a 200 back never mistakes it for "all of it arrived".
    char out[224];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"accepted\":%d,\"stored\":%d,\"queued\":%d,"
             "\"rejected\":%d,\"held\":%s,\"room\":%d,"
             "\"clock_rejected\":%s}",
             accepted, stored, queued, rejected,
             backpressure ? "true" : "false", remoteIngest.historyRoom(),
             clockRejected ? "true" : "false");
    req->send(200, "application/json", out);
}

void registerIngestHandler(AsyncWebServer& server) {
    server.on("/api/ingest", HTTP_POST,
              [](AsyncWebServerRequest* r) { /* handled in the body callback */ },
              nullptr,
              handleIngestBody);
}

#endif  // FEATURE_REMOTE_NODES
