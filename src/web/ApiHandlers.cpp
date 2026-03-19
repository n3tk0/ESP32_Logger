#include "ApiHandlers.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/task.h>
#include "../pipeline/DataPipeline.h"
#include "../pipeline/AggregationEngine.h"
#include "../sensors/SensorManager.h"
#include "../export/ExportManager.h"
#include "../storage/JsonLogger.h"
#include "../core/Globals.h"   // config, activeFS
#include "../tasks/TaskManager.h"  // task handles for /api/diag

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

    // 2) Filesystem query — choose strategy based on whether ring has data:
    //    a) Ring is empty (historical query): use streaming aggregation (P1/3.1)
    //       — avoids materialising raw readings, saving ~40KB heap.
    //    b) Ring has data (recent query): use raw query + merge for ring+FS union.
    bool historicalPath = (ringCount == 0) &&
                          (mode != AGG_RAW) &&
                          (bucket != BUCKET_RAW);

    SensorReading* agg     = new SensorReading[limit + 1];
    size_t         aggCount = 0;
    bool           truncated = false;

    if (!agg) {
        delete[] raw;
        req->send(500, "application/json", "{\"error\":\"out of memory\"}");
        return;
    }

    if (historicalPath && activeFS) {
        // Streaming path: Accum[] allocated inside, freed before return (P1/3.1)
        static JsonLogger logger;
        if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            aggCount = logger.streamAggregateQuery(*activeFS, fromTs, toTs,
                                                    sensorFilter, metricFilter,
                                                    agg, limit,
                                                    bucket, mode, limit);
            xSemaphoreGive(configMutex);
        }
        delete[] raw;  // ring buffer was empty; free it
        raw = nullptr;
    } else {
        // Raw path: query FS for remaining slots, merge with ring, then aggregate
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
        truncated = (rawCount >= MAX_RAW);

        // Merge: insertion-sort by timestamp + dedup by (ts, sensorId, metric) (#4)
        if (fsCount > 0 && ringCount > 0) {
            for (size_t i = ringCount; i < rawCount; i++) {
                SensorReading tmp = raw[i];
                size_t j = i;
                while (j > 0 && raw[j-1].timestamp > tmp.timestamp) {
                    raw[j] = raw[j-1]; j--;
                }
                raw[j] = tmp;
            }
            size_t out = 0;
            for (size_t i = 0; i < rawCount; i++) {
                bool dup = (i > 0 &&
                            raw[i].timestamp == raw[i-1].timestamp &&
                            strcmp(raw[i].sensorId, raw[i-1].sensorId) == 0 &&
                            strcmp(raw[i].metric,   raw[i-1].metric)   == 0);
                if (!dup) { if (out != i) raw[out] = raw[i]; out++; }
            }
            rawCount = out;
        }

        if (rawCount > 0) {
            aggCount = AggregationEngine::aggregate(raw, rawCount,
                                                    agg, limit,
                                                    bucket, mode, limit);
        }
        delete[] raw;
        raw = nullptr;
    }

    // 4.3 — Warn when multiple metrics are mixed in one response without a filter
    bool multiMetricWarning = false;
    if (!metricFilter && aggCount > 1) {
        const char* firstMetric = agg[0].metric;
        for (size_t i = 1; i < aggCount; i++) {
            if (strcmp(agg[i].metric, firstMetric) != 0) {
                multiMetricWarning = true;
                break;
            }
        }
    }

    // --- Build JSON response via AsyncResponseStream (no String realloc) ---
    AsyncResponseStream* response =
        req->beginResponseStream("application/json");
    response->printf("{\"from\":%u,\"to\":%u,\"agg\":\"%s\",\"mode\":\"%s\","
                     "\"count\":%zu,\"truncated\":%s",
                     fromTs, toTs, aggParamStr, modeParamStr,
                     aggCount, truncated ? "true" : "false");
    if (multiMetricWarning) {
        response->print(",\"warning\":\"multiple metrics in response; add metric= "
                        "param for single-series queries\"");
    }
    response->print(",\"data\":[");

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
// GET /api/diag — FreeRTOS diagnostics: heap, queues, task stack HWMs, drops
// ---------------------------------------------------------------------------
static void handleApiDiag(AsyncWebServerRequest* req) {
    StaticJsonDocument<1024> doc;

    // Heap
    doc["free_heap"]     = (uint32_t)ESP.getFreeHeap();
    doc["min_free_heap"] = (uint32_t)ESP.getMinFreeHeap();
    doc["queue_drops"]   = (uint32_t)g_queueDrops;

    // Queues
    JsonObject queues = doc.createNestedObject("queues");
    if (sensorQueue) {
        JsonObject q = queues.createNestedObject("sensor");
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(sensorQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(sensorQueue);
    }
    if (storageQueue) {
        JsonObject q = queues.createNestedObject("storage");
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(storageQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(storageQueue);
    }
    if (exportQueue) {
        JsonObject q = queues.createNestedObject("export");
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(exportQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(exportQueue);
    }

    // Task stack high-water marks (words remaining before overflow)
    JsonObject tasks = doc.createNestedObject("tasks");
    if (TaskManager::hSensor)
        tasks["SensorTask"]     = (uint32_t)uxTaskGetStackHighWaterMark(TaskManager::hSensor);
    if (TaskManager::hSlowSensor)
        tasks["SlowSensorTask"] = (uint32_t)uxTaskGetStackHighWaterMark(TaskManager::hSlowSensor);
    if (TaskManager::hProcess)
        tasks["ProcessTask"]    = (uint32_t)uxTaskGetStackHighWaterMark(TaskManager::hProcess);
    if (TaskManager::hStorage)
        tasks["StorageTask"]    = (uint32_t)uxTaskGetStackHighWaterMark(TaskManager::hStorage);
    if (TaskManager::hExport)
        tasks["ExportTask"]     = (uint32_t)uxTaskGetStackHighWaterMark(TaskManager::hExport);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/data",             HTTP_GET,  handleApiData);
    server.on("/api/sensors",          HTTP_GET,  handleApiSensors);
    server.on("/api/diag",             HTTP_GET,  handleApiDiag);
    server.on("/api/config/platform",  HTTP_POST, handleConfigPlatform);
}
