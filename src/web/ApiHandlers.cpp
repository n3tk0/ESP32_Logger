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

    size_t rawCount = 0;

    // Try ring buffer first (no mutex needed for read-only snapshot)
    if (xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        rawCount = webRingBuf.copyRecent(raw, MAX_RAW, fromTs);
        xSemaphoreGive(webDataMutex);
    }

    // Apply sensor/metric/toTs filters to ring buffer results
    if (rawCount > 0) {
        size_t out = 0;
        for (size_t i = 0; i < rawCount; i++) {
            if (raw[i].timestamp > toTs) continue;
            if (sensorFilter && strcmp(raw[i].sensorId, sensorFilter) != 0) continue;
            if (metricFilter && strcmp(raw[i].metric,   metricFilter) != 0) continue;
            if (out != i) raw[out] = raw[i];
            out++;
        }
        rawCount = out;
    }

    // Capture agg/mode strings before any async ops
    const char* aggParamStr  = req->hasParam("agg")  ? req->getParam("agg")->value().c_str()  : "5m";
    const char* modeParamStr = req->hasParam("mode") ? req->getParam("mode")->value().c_str() : "lttb";

    // If ring buffer doesn't cover the requested range, query filesystem.
    // Guard with configMutex: static JsonLogger shares internal state and
    // concurrent /api/data requests would race on openNextFile().
    bool truncated = false;
    if (rawCount == 0 && activeFS) {
        static JsonLogger logger;
        if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            rawCount = logger.query(*activeFS, fromTs, toTs,
                                    sensorFilter, metricFilter,
                                    raw, MAX_RAW);
            xSemaphoreGive(configMutex);
        }
    }
    truncated = (rawCount >= MAX_RAW);

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
        response->printf("%s{\"ts\":%u,\"v\":%s}",
                         (i > 0 ? "," : ""), agg[i].timestamp, valBuf);
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
