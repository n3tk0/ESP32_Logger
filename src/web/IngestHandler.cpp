#include "IngestHandler.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <new>                        // nothrow, for the body buffer
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

/// Parse and act on one complete request body. Called by the segment callback
/// below once every byte has arrived.
static void handleIngestPayload(AsyncWebServerRequest* req,
                                const uint8_t* data, size_t len) {
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
    // has three times the room, so it is told to wait instead: the loop below
    // stops at the first backfill reading there is no space for, and
    // everything from there on stays where it already is.
    //
    // A reading the collector cannot use — no metric name, a value that is not
    // a number, a date before 2001 — is consumed rather than blocking the
    // queue behind it. Sending it again would fail the same way for ever.
    int stored = 0, queued = 0, rejected = 0, accepted = 0;
    bool backpressure = false;

    // ── The current values first, and outside the prefix rule entirely ──────
    //
    // THE HISTORY QUEUE IS ONE QUEUE FOR EVERY NODE, and it is drained by each
    // node's own sensor plugin. So a node that posts with a valid token but
    // has no `remote` sensor configured for it fills those 64 slots with
    // entries nothing will ever drain — and then historyRoom() is zero for
    // ever, for everybody.
    //
    // A batch is sent oldest first, so its first reading is backfill and the
    // loop below would break on it immediately: accepted would be 0 and the
    // CURRENT readings at the end of the batch would never be reached. Every
    // other node would then read as offline on the dashboard, on /api/nodes
    // and in "last seen" while posting perfectly, because nothing was
    // refreshing its mailbox.
    //
    // The mailbox needs no room — it is one slot per (node, metric), already
    // allocated — so it is filled here, before any of that can apply. A live
    // reading the prefix does not reach stays in the node's ring and is
    // offered again next cycle; put() is a mailbox and writing it twice is
    // writing it once.
    {
        int i = -1;
        for (JsonObjectConst r : readings) {
            i++;
            if (!IngestBatch::isLive(IngestBatch::isNewest(liveIdx, nLive, i),
                                     IngestBatch::clampAge(r["dt_s"] | 0UL)))
                continue;
            const char* metric = r["metric"] | "";
            if (*metric == '\0' || !r["value"].is<float>()) continue;
            const float value = r["value"].as<float>();
            if (!isfinite(value)) continue;
            const uint32_t age = IngestBatch::clampAge(r["dt_s"] | 0UL);
            if (remoteIngest.put(node, metric, value, r["unit"] | "",
                                 (canBackfill && base > age) ? base - age : ts))
                stored++;
        }
    }

    int idx = -1;
    for (JsonObjectConst r : readings) {
        idx++;
        const char* metric = r["metric"] | "";
        const char* unit   = r["unit"]   | "";
        // No default: an absent value must be rejected, not read as 0.
        const bool  hasValue = r["value"].is<float>();
        const float value    = hasValue ? r["value"].as<float>() : 0.0f;

        const uint32_t age  = IngestBatch::clampAge(r["dt_s"] | 0UL);
        const bool     live = IngestBatch::isLive(
                                  IngestBatch::isNewest(liveIdx, nLive, idx), age);
        const bool     backfill =
            IngestBatch::isBackfill(live, age, canBackfill, base);
        const uint32_t when = backfill ? (base - age) : 0;

        // ROOM CHECKED BEFORE THE READING IS JUDGED, so that a batch stops at
        // the same place whatever it happens to contain. Deciding to stop is
        // about the queue, not about this reading.
        if (backfill && remoteIngest.historyRoom() <= 0) {
            backpressure = true;
            break;
        }

        accepted++;

        // Already in the mailbox, from the pass above. Counted there.
        if (live) continue;

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
            // Not the newest of its metric and not old enough to be history:
            // a second reading taken in the same second. The mailbox keeps the
            // time it was TAKEN, not the time it arrived.
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

// ── One body, however many TCP segments it arrives in ───────────────────────
//
// THIS USED TO REFUSE ANYTHING THAT DID NOT ARRIVE IN ONE PIECE, and while the
// cap was 1 KB that was very nearly always true. It stopped being true the
// moment the cap went to 4 KB for buffered batches: ESPAsyncWebServer hands
// over a body one TCP segment at a time, the ESP32's MSS is about 1.4 KB, and
// a batch of forty-eight readings is roughly 2.9 KB. So every batch large
// enough to need the new cap was answered 413 — and the node holds a batch on
// any non-200, which means a node that fell far enough behind to send a large
// batch could never send anything again. The feature would have failed exactly
// when it was needed.
//
// Accumulating through _tempObject is what /api/firstrun and
// /api/kindle/slots already do, and the shape is theirs: allocate on the
// opening segment, register the disconnect cleaner immediately after (a client
// that drops mid-body would otherwise orphan the buffer, because the delete at
// the end never runs), and answer once, when the last byte is in.
static void handleIngestBody(AsyncWebServerRequest* req, uint8_t* data,
                             size_t len, size_t index, size_t total) {
    // The size check answers only on the opening segment. Every req->send()
    // overwrites the request's response object — leaking the previous one —
    // and writes another HTTP response onto the same socket, so replying per
    // segment corrupts the connection.
    if (total > INGEST_MAX_BODY) {
        if (index == 0) {
            req->send(413, "application/json",
                      "{\"ok\":false,\"error\":\"body too large\"}");
        }
        return;
    }

    if (index == 0) {
        req->_tempObject = new (std::nothrow) String();
        if (!req->_tempObject) {
            req->send(500, "application/json",
                      "{\"ok\":false,\"error\":\"out of memory\"}");
            return;
        }
        req->onDisconnect([req]() {
            delete static_cast<String*>(req->_tempObject);
            req->_tempObject = nullptr;
        });
        static_cast<String*>(req->_tempObject)->reserve(total);
    }

    String* buf = static_cast<String*>(req->_tempObject);
    if (!buf) return;                     // the opening segment failed to allocate
    buf->concat(reinterpret_cast<const char*>(data), len);

    if (index + len >= total) {
        handleIngestPayload(req, (const uint8_t*)buf->c_str(), buf->length());
        delete buf;
        req->_tempObject = nullptr;
    }
}

void registerIngestHandler(AsyncWebServer& server) {
    server.on("/api/ingest", HTTP_POST,
              [](AsyncWebServerRequest* r) { /* handled in the body callback */ },
              nullptr,
              handleIngestBody);
}

#endif  // FEATURE_REMOTE_NODES
