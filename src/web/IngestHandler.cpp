#include "IngestHandler.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "RateLimiter.h"
#include "../sensors/RemoteIngest.h"

#ifndef INGEST_TOKEN
#  define INGEST_TOKEN "change-me"
#endif

// A node payload is a handful of small objects. Anything larger is either a
// misconfigured client or someone probing, and buffering it would be the
// only unbounded allocation on this path.
static constexpr size_t INGEST_MAX_BODY = 1024;

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

    // Stored, but not authoritative: SensorManager stamps the collector's
    // clock over every reading once the plugin returns, remote or wired. The
    // reference node has no RTC and sends 0, so nothing is lost today — but
    // do not read this field as "the node's sampling time is preserved".
    // See RemoteIngest::put().
    const uint32_t ts = body["ts"] | 0UL;

    int stored = 0, rejected = 0;
    for (JsonObjectConst r : readings) {
        const char* metric = r["metric"] | "";
        const char* unit   = r["unit"]   | "";
        // No default: an absent value must be rejected, not read as 0.
        if (!r["value"].is<float>()) { rejected++; continue; }
        const float value = r["value"].as<float>();

        if (remoteIngest.put(node, metric, value, unit, ts)) stored++;
        else                                                 rejected++;
    }

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"stored\":%d,\"rejected\":%d}",
             stored, rejected);
    req->send(200, "application/json", out);
}

void registerIngestHandler(AsyncWebServer& server) {
    server.on("/api/ingest", HTTP_POST,
              [](AsyncWebServerRequest* r) { /* handled in the body callback */ },
              nullptr,
              handleIngestBody);
}

#endif  // FEATURE_REMOTE_NODES
