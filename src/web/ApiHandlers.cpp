#include "ApiHandlers.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../pipeline/DataPipeline.h"
#include "../pipeline/AggregationEngine.h"
#include "../sensors/SensorManager.h"
#include "../export/ExportManager.h"
#include "../storage/JsonLogger.h"
#include "../core/Globals.h"   // config, activeFS

// ---------------------------------------------------------------------------
// GET /api/data
//   from=    Unix timestamp (default: now - 86400)
//   to=      Unix timestamp (default: now)
//   sensor=  sensorId filter (optional)
//   metric=  metric filter (optional)
//   agg=     raw|1m|5m|1h|1d (default 5m)
//   mode=    raw|avg|min|max|lttb (default lttb)
//   limit=   max output points (default 500)
// ---------------------------------------------------------------------------
static void handleApiData(AsyncWebServerRequest* req) {
    uint32_t now = (uint32_t)(millis() / 1000UL); // fallback

    uint32_t fromTs = req->hasParam("from")
                      ? (uint32_t)req->getParam("from")->value().toInt()
                      : (now - 86400);
    uint32_t toTs   = req->hasParam("to")
                      ? (uint32_t)req->getParam("to")->value().toInt()
                      : now;

    const char* sensorFilter = req->hasParam("sensor")
                               ? req->getParam("sensor")->value().c_str()
                               : nullptr;
    const char* metricFilter = req->hasParam("metric")
                               ? req->getParam("metric")->value().c_str()
                               : nullptr;

    TimeBucket bucket = parseBucket(req->hasParam("agg")
                        ? req->getParam("agg")->value().c_str() : "5m");
    AggMode    mode   = parseMode(req->hasParam("mode")
                        ? req->getParam("mode")->value().c_str() : "lttb");
    size_t     limit  = req->hasParam("limit")
                        ? (size_t)req->getParam("limit")->value().toInt()
                        : 500;
    if (limit < 1 || limit > 5000) limit = 500;

    // --- Fetch raw data ---
    // Strategy: first try in-memory ring buffer (recent data),
    //           fall back to filesystem query for historical data.
    constexpr size_t MAX_RAW = 500;  // 40 KB vs 160 KB — prevents OOM on ESP32-C3
    SensorReading* raw = new SensorReading[MAX_RAW];
    if (!raw) {
        req->send(500, "application/json", "{\"error\":\"out of memory\"}");
        return;
    }

    // Strategy: always query BOTH ring buffer (up to RING_SHARE) AND filesystem
    // (up to FS_SHARE), then merge by timestamp and deduplicate (#4).
    constexpr size_t RING_SHARE = 200;
    constexpr size_t FS_SHARE   = MAX_RAW - RING_SHARE;  // = 300

    size_t ringCount = 0;
    size_t fsCount   = 0;

    // 1) Ring buffer (recent, in-memory)
    if (xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ringCount = webRingBuf.copyRecent(raw, RING_SHARE, fromTs);
        xSemaphoreGive(webDataMutex);
    }
    // Filter ring results
    {
        size_t out = 0;
        for (size_t i = 0; i < ringCount; i++) {
            if (raw[i].timestamp > toTs) continue;
            if (sensorFilter && strcmp(raw[i].sensorId, sensorFilter) != 0) continue;
            if (metricFilter && strcmp(raw[i].metric,   metricFilter) != 0) continue;
            if (out != i) raw[out] = raw[i];
            out++;
        }
        ringCount = out;
    }

    // Capture agg/mode strings before any async ops
    const char* aggParamStr  = req->hasParam("agg")  ? req->getParam("agg")->value().c_str()  : "5m";
    const char* modeParamStr = req->hasParam("mode") ? req->getParam("mode")->value().c_str() : "lttb";

    // 2) Filesystem query (historical); always run alongside ring (#4)
    if (activeFS) {
        static JsonLogger logger;
        if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            fsCount = logger.query(*activeFS, fromTs, toTs,
                                   sensorFilter, metricFilter,
                                   raw + ringCount, FS_SHARE);
            xSemaphoreGive(configMutex);
        }
    }

    size_t rawCount = ringCount + fsCount;

    // 3) Merge: sort combined buffer by timestamp, then dedup consecutive
    //    entries with identical (timestamp, sensorId, metric) (#4)
    if (fsCount > 0 && ringCount > 0) {
        // Simple insertion sort — buffer is at most 500 entries
        for (size_t i = ringCount; i < rawCount; i++) {
            SensorReading tmp = raw[i];
            size_t j = i;
            while (j > 0 && raw[j-1].timestamp > tmp.timestamp) {
                raw[j] = raw[j-1];
                j--;
            }
            raw[j] = tmp;
        }
        // Dedup: keep only the first occurrence of (ts, sensorId, metric)
        size_t out = 0;
        for (size_t i = 0; i < rawCount; i++) {
            bool dup = false;
            if (i > 0 &&
                raw[i].timestamp == raw[i-1].timestamp &&
                strcmp(raw[i].sensorId, raw[i-1].sensorId) == 0 &&
                strcmp(raw[i].metric,   raw[i-1].metric)   == 0) {
                dup = true;
            }
            if (!dup) {
                if (out != i) raw[out] = raw[i];
                out++;
            }
        }
        rawCount = out;
    }

    bool truncated = (rawCount >= MAX_RAW);

    // --- Aggregate ---
    SensorReading* agg = new SensorReading[limit + 1];
    size_t aggCount = 0;
    if (agg && rawCount > 0) {
        aggCount = AggregationEngine::aggregate(raw, rawCount,
                                                agg, limit,
                                                bucket, mode, limit);
    }

    delete[] raw;

    // --- Build JSON response via AsyncResponseStream (no String realloc) ---
    AsyncResponseStream* response =
        req->beginResponseStream("application/json");
    response->printf("{\"from\":%u,\"to\":%u,\"agg\":\"%s\",\"mode\":\"%s\","
                     "\"count\":%zu,\"truncated\":%s,\"data\":[",
                     fromTs, toTs, aggParamStr, modeParamStr,
                     aggCount, truncated ? "true" : "false");

    for (size_t i = 0; i < aggCount; i++) {
        char valBuf[16];
        snprintf(valBuf, sizeof(valBuf), "%.4g", agg[i].value);
        // Include metric and unit so clients can display axes correctly (#12)
        response->printf("%s{\"ts\":%u,\"v\":%s,\"metric\":\"%s\",\"unit\":\"%s\"}",
                         (i > 0 ? "," : ""),
                         agg[i].timestamp, valBuf,
                         agg[i].metric, agg[i].unit);
    }
    response->print("]}");

    delete[] agg;
    req->send(response);
}

// ---------------------------------------------------------------------------
// GET /api/sensors — list registered sensors + status
// ---------------------------------------------------------------------------
static void handleApiSensors(AsyncWebServerRequest* req) {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.createNestedArray("sensors");
    sensorManager.toJson(arr);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// POST /api/config/platform — reload platform_config.json
// ---------------------------------------------------------------------------
static void handleConfigPlatform(AsyncWebServerRequest* req) {
    if (!activeFS) {
        req->send(503, "application/json", "{\"error\":\"no fs\"}");
        return;
    }
    // Lock config mutex so tasks don't read a partially-updated config
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        bool sensorsOk   = sensorManager.reloadConfig(*activeFS);
        bool exportersOk = exportManager.reloadConfig(*activeFS);
        xSemaphoreGive(configMutex);
        if (sensorsOk && exportersOk) req->send(200, "application/json", "{\"ok\":true}");
        else                          req->send(500, "application/json", "{\"error\":\"reload failed\"}");
    } else {
        req->send(503, "application/json", "{\"error\":\"busy\"}");
    }
}

// ---------------------------------------------------------------------------
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/data",             HTTP_GET,  handleApiData);
    server.on("/api/sensors",          HTTP_GET,  handleApiSensors);
    server.on("/api/config/platform",  HTTP_POST, handleConfigPlatform);
}
