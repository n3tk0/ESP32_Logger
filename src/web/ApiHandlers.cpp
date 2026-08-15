#include "ApiHandlers.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>                      // WiFi scan/test (Pass 5 5.5 phase 1)
#include <time.h>                      // /api/backup created_at (Pass 5 5.7)
#include <freertos/task.h>
#include <new>                            // std::nothrow
#include <atomic>
#include "../pipeline/DataPipeline.h"
#include "../pipeline/AggregationEngine.h"
#include "../sensors/SensorManager.h"
#include "../export/ExportManager.h"
#include "../export/MqttExporter.h"
// Historical sensor data is now persisted as wide CSV by StorageTask
// (see src/pipeline/LiveAggregator + src/storage/CsvLogger).  The CSV
// query/streaming endpoint that replaces JsonLogger lands with the
// Smart Dashboard rework (chunk F).  Until then, /api/data serves
// the in-memory ring buffer only — historical FS queries return 0 rows.
#include "../core/Globals.h"         // config, activeFS
#include "../core/ModuleRegistry.h"  // Pass 5 phase 3: /api/modules
#include "../managers/ConfigManager.h" // saveConfig() after module update
#include "RateLimiter.h"               // Pass 7 rate-limit on mutating routes
#include "RequireAuth.h"               // R5: unified mutating-handler auth preamble
#include "../utils/JsonResponse.h"     // R9: sendJsonResponse helper
#include "../alerts/AlertEngine.h"    // GET/POST /api/alerts, snooze, toasts
#include <Wire.h>                     // POST /api/i2c_scan
#include "../sensors/I2CBus.h"        // bus registry for /api/i2c_scan
// Forward-declared in Logger.ino — accessible here because this file is
// compiled in the same sketch scope.
#ifdef EXPORT_MQTT_ENABLED
extern MqttExporter* g_mqttExporter;
#endif
#include "../tasks/TaskManager.h"  // task handles for /api/diag
#include "../managers/OtaManager.h"
#include "../utils/MutexGuard.h"   // R19.D: guarded /reset_log.txt read

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

    // Copy filter strings to local buffers — AsyncWebParameter::value() is a
    // String whose c_str() may dangle after the param object is freed during
    // async response streaming.  (AUDIT 3.10)
    char sensorFilterBuf[33] = "";
    char metricFilterBuf[24] = "";
    if (req->hasParam("sensor")) {
        strncpy(sensorFilterBuf, req->getParam("sensor")->value().c_str(),
                sizeof(sensorFilterBuf) - 1);
    }
    if (req->hasParam("metric")) {
        strncpy(metricFilterBuf, req->getParam("metric")->value().c_str(),
                sizeof(metricFilterBuf) - 1);
    }
    const char* sensorFilter = sensorFilterBuf[0] ? sensorFilterBuf : nullptr;
    const char* metricFilter = metricFilterBuf[0] ? metricFilterBuf : nullptr;

    TimeBucket bucket = parseBucket(req->hasParam("agg")
                        ? req->getParam("agg")->value().c_str() : "5m");
    AggMode    mode   = parseMode(req->hasParam("mode")
                        ? req->getParam("mode")->value().c_str() : "lttb");
    // CM-2: parse + validate as a SIGNED long first.  Casting toInt() straight
    // to size_t turned a negative "limit" into a multi-GB value that the
    // `< 1` guard could never catch; validate the sign before the cast.
    long limitRaw = req->hasParam("limit")
                    ? req->getParam("limit")->value().toInt()
                    : 250;
    if (limitRaw < 1)   limitRaw = 250;
    if (limitRaw > 300) limitRaw = 300; // Cap to 300 to prevent OOM on ESP32-C3 (~24KB)
    size_t limit = (size_t)limitRaw;

    // --- Fetch raw data ---
    // Strategy: first try in-memory ring buffer (recent data),
    //           fall back to filesystem query for historical data.
    constexpr size_t MAX_RAW = 300;  // ~20 KB — prevents OOM on ESP32-C3
    SensorReading* raw = new(std::nothrow) SensorReading[MAX_RAW];
    if (!raw) {
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
        return;
    }

    // Slots given to the ring buffer. The split with the filesystem query is
    // moot while that path is disabled (see "chunk F" below, fsCount = 0), so
    // the ring gets the whole budget instead of leaving 100 slots reserved for
    // rows that never arrive. Restore a split when the CSV reader lands.
    //
    // NOTE: this bounds chart depth to MAX_RAW readings regardless of how deep
    // the ring itself is. A PSRAM-backed ring holds tens of thousands of
    // entries, but a single /api/data request still only sees the newest
    // MAX_RAW of them — roughly 2.5 min for a build emitting ~19 metrics every
    // 10 s. Serving hours in one request needs aggregation that accumulates
    // per bucket while scanning the ring, rather than materialising raw
    // readings into this array first; that is the same work chunk F implies
    // for the FS side and is deliberately not attempted here.
    constexpr size_t RING_SHARE = MAX_RAW;

    size_t ringCount = 0;
    size_t fsCount   = 0;

    // 1) Ring buffer (recent, in-memory)
    if (webDataMutex && xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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

    // Copy agg/mode strings — c_str() pointers may dangle during async response (N21)
    char aggParamBuf[16]  = "5m";
    char modeParamBuf[16] = "lttb";
    if (req->hasParam("agg"))  { strncpy(aggParamBuf,  req->getParam("agg")->value().c_str(),  sizeof(aggParamBuf) - 1);  aggParamBuf[sizeof(aggParamBuf) - 1]   = '\0'; }
    if (req->hasParam("mode")) { strncpy(modeParamBuf, req->getParam("mode")->value().c_str(), sizeof(modeParamBuf) - 1); modeParamBuf[sizeof(modeParamBuf) - 1] = '\0'; }
    const char* aggParamStr  = aggParamBuf;
    const char* modeParamStr = modeParamBuf;

    // 2) Filesystem query — choose strategy based on whether ring has data:
    //    a) Ring is empty (historical query): use streaming aggregation (P1/3.1)
    //       — avoids materialising raw readings, saving ~40KB heap.
    //    b) Ring has data (recent query): use raw query + merge for ring+FS union.
    bool historicalPath = (ringCount == 0) &&
                          (mode != AGG_RAW) &&
                          (bucket != BUCKET_RAW);

    SensorReading* agg    = nullptr;
    size_t         aggCount = 0;
    bool           truncated = false;

    if (historicalPath && activeFS) {
        // FS-backed history is migrating to wide CSV — chunk F will land a
        // dedicated CSV-streaming endpoint.  Until then this path returns
        // zero historical rows; recent data still flows from the ring buffer.
        delete[] raw;
        raw = nullptr;
    } else {
        agg = new(std::nothrow) SensorReading[limit + 1];
        if (!agg) {
            delete[] raw;
            req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
            return;
        }
        // FS query disabled until the wide-CSV reader ships (chunk F).
        fsCount = 0;

        size_t rawCount = ringCount + fsCount;
        // Report truncation when the ring hit its share too, not just when the
        // combined array filled. With RING_SHARE == MAX_RAW these coincide, but
        // stating both keeps the flag honest if the split is reintroduced —
        // otherwise the response claims completeness while the ring still holds
        // readings inside the requested range.
        truncated = (rawCount >= MAX_RAW) || (ringCount >= RING_SHARE);

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
// GET /api/latest
//   Smart Dashboard polling endpoint.  Returns the most recent reading per
//   (sensorId, metric) tuple seen in the in-memory webRingBuf, plus the
//   sensor's display name from SensorManager.  Designed to be cheap enough
//   to poll every 60 s from the dashboard:
//   - single mutex acquire
//   - scan bounded by MAX_RAW (500) readings copied out of the ring, NOT by
//     the ring's own capacity — which is now a runtime value and, on a PSRAM
//     board, tens of thousands of entries. copyRecent() stops at maxOut, so
//     the cost of this endpoint does not grow with the ring.
//   - JsonDocument sized for ≤ 32 (sensor, metric) pairs
//
// Response shape:
//   { "ok": true, "ts": 1714900000,
//     "items": [
//       { "id": "env_indoor", "type": "bme280", "metric": "temperature",
//         "value": 22.5, "unit": "C", "ts": 1714900000, "q": 1 },
//       ...
//     ] }
// ---------------------------------------------------------------------------
static void handleApiLatest(AsyncWebServerRequest* req) {
    constexpr size_t MAX_RAW = 500;
    SensorReading* raw = new (std::nothrow) SensorReading[MAX_RAW];
    if (!raw) {
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
        return;
    }

    size_t copied = 0;
    if (webDataMutex && xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copied = webRingBuf.copyRecent(raw, MAX_RAW, 0);
        xSemaphoreGive(webDataMutex);
    }

    // Walk newest→oldest, keeping the first occurrence of each (id, metric).
    // 32 unique pairs is well above the realistic device sensor count.
    constexpr size_t MAX_PAIRS = 32;
    int    latestIdx[MAX_PAIRS];
    size_t nPairs = 0;

    if (copied > 0) {
        for (int i = (int)copied - 1; i >= 0 && nPairs < MAX_PAIRS; --i) {
            const SensorReading& r = raw[i];
            bool seen = false;
            for (size_t j = 0; j < nPairs; j++) {
                const SensorReading& p = raw[latestIdx[j]];
                if (strcmp(p.sensorId, r.sensorId) == 0 &&
                    strcmp(p.metric,   r.metric)   == 0) { seen = true; break; }
            }
            if (!seen) latestIdx[nPairs++] = i;
        }
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["ts"] = (uint32_t)(millis() / 1000UL);
    JsonArray arr = doc["items"].to<JsonArray>();
    for (size_t k = 0; k < nPairs; k++) {
        const SensorReading& r = raw[latestIdx[k]];
        JsonObject o = arr.add<JsonObject>();
        o["id"]     = r.sensorId;
        o["type"]   = r.sensorType;
        o["metric"] = r.metric;
        o["value"]  = r.value;
        o["unit"]   = r.unit;
        o["ts"]     = r.timestamp;
        o["q"]      = (uint8_t)r.quality;
    }

    delete[] raw;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// GET /api/sensors — list registered sensors + status
// ---------------------------------------------------------------------------
static void handleApiSensors(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["sensors"].to<JsonArray>();
    sensorManager.toJson(arr);

    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/config/platform — reload platform_config.json
// ---------------------------------------------------------------------------
static void handleConfigPlatform(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF
    if (!activeFS) {
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
        return;
    }
    // Lock config mutex so tasks don't read a partially-updated config
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        bool sensorsOk   = sensorManager.reloadConfig(*activeFS);
        bool exportersOk = exportManager.reloadConfig(*activeFS);
        // Propagate StorageTask-visible knobs (SDS011 humidity correction).
        // StorageTask re-reads storageParam every aggregation tick, so this
        // is enough to apply changes live without a reboot.
        TaskManager::refreshStorageFromPlatform(*activeFS);
        xSemaphoreGive(configMutex);
        if (sensorsOk && exportersOk) req->send(200, "application/json", "{\"ok\":true}");
        else                          req->send(500, "application/json", "{\"ok\":false,\"error\":\"reload failed\"}");
    } else {
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    }
}

// ---------------------------------------------------------------------------
// GET /api/diag — FreeRTOS diagnostics: heap, queues, task stack HWMs, drops
// ---------------------------------------------------------------------------
static void handleApiDiag(AsyncWebServerRequest* req) {
    JsonDocument doc;

    // Heap (legacy top-level fields kept for backwards compat)
    doc["free_heap"]     = (uint32_t)ESP.getFreeHeap();
    doc["min_free_heap"] = (uint32_t)ESP.getMinFreeHeap();
    doc["queue_drops"]   = (uint32_t)g_queueDrops;

    // R19.A — heap sub-object (snapshot all values once for consistency)
    {
        uint32_t fr = ESP.getFreeHeap();
        uint32_t mn = ESP.getMinFreeHeap();
        uint32_t lg = ESP.getMaxAllocHeap();
        JsonObject heap = doc["heap"].to<JsonObject>();
        heap["free"]         = fr;
        heap["min"]          = mn;
        heap["largestBlock"] = lg;
        int pct = (fr > 0) ? (int)(100 - (100ULL * lg) / fr) : 0;
        if (pct < 0) pct = 0;
        heap["fragPct"] = pct;
    }

    // Queues
    // PSRAM + ring-buffer state. First stop when the dashboard shows less
    // history than expected: "size" of 0 on a board that has PSRAM fitted
    // almost always means the build is compiled for the wrong SPI mode
    // (octal parts need board_build.arduino.memory_type = qio_opi), and the
    // ring then silently falls back to the small internal-RAM budget.
    {
        JsonObject psram = doc["psram"].to<JsonObject>();
        psram["size"] = (uint32_t)ESP.getPsramSize();
        psram["free"] = (uint32_t)ESP.getFreePsram();

        JsonObject ring = doc["ring"].to<JsonObject>();
        ring["capacity"] = (uint32_t)webRingBuf.capacity();
        ring["used"]     = (uint32_t)webRingBuf.size();
        ring["bytes"]    = (uint32_t)(webRingBuf.capacity() * sizeof(SensorReading));
        ring["psram"]    = webRingBuf.isPsram();
    }

    // I2C bus state — which controllers this chip has and which the current
    // sensor config actually brought up. The first thing to check when a
    // sensor on a second bus reports "not found": an unconfigured bus 1 means
    // no sensor claimed it, and a bus count of 1 means the chip cannot have it.
    {
        JsonObject i2c = doc["i2c"].to<JsonObject>();
        i2c["controllers"] = I2CBus::hardwareBusCount();
        JsonArray buses = i2c["buses"].to<JsonArray>();
        for (uint8_t b = 0; b < I2CBus::hardwareBusCount(); b++) {
            JsonObject o = buses.add<JsonObject>();
            o["bus"] = b;
            o["up"]  = I2CBus::isConfigured(b);
            if (I2CBus::isConfigured(b)) {
                o["sda"] = I2CBus::sdaOf(b);
                o["scl"] = I2CBus::sclOf(b);
            }
        }
    }

    JsonObject queues = doc["queues"].to<JsonObject>();
    if (sensorQueue) {
        JsonObject q = queues["sensor"].to<JsonObject>();
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(sensorQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(sensorQueue);
    }
    if (storageQueue) {
        JsonObject q = queues["storage"].to<JsonObject>();
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(storageQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(storageQueue);
    }
    if (exportQueue) {
        JsonObject q = queues["export"].to<JsonObject>();
        q["waiting"] = (uint32_t)uxQueueMessagesWaiting(exportQueue);
        q["spaces"]  = (uint32_t)uxQueueSpacesAvailable(exportQueue);
    }

    // Task stack high-water marks (words remaining before overflow)
    JsonObject tasks = doc["tasks"].to<JsonObject>();
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
    // R19.B — camelCase aliases (value = words remaining; ×4 → bytes on 32-bit)
    auto stackWords = [](TaskHandle_t h) -> uint32_t {
        return h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0;
    };
    tasks["sensor"]     = stackWords(TaskManager::hSensor);
    tasks["slowSensor"] = stackWords(TaskManager::hSlowSensor);
    tasks["process"]    = stackWords(TaskManager::hProcess);
    tasks["storage"]    = stackWords(TaskManager::hStorage);
    tasks["export"]     = stackWords(TaskManager::hExport);

    // R19.C — drop and reset counters
    {
        JsonObject c = doc["counters"].to<JsonObject>();
        c["queueDrops"]    = (uint32_t)g_queueDrops;
        c["ringPushDrops"] = g_ringPushDrops.load();
        c["resets"]        = (uint32_t)g_consecutiveResets;
    }

    // OTA rollback info
    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["running"]          = OtaManager::runningPartitionLabel();
    ota["previous"]         = OtaManager::previousPartitionLabel();
    ota["pending_verify"]   = OtaManager::isPendingVerify();
    ota["rollback_capable"] = OtaManager::isRollbackCapable();

    // uptime + network.ip for the failsafe banner (and general observability)
    doc["uptime"] = (uint32_t)(millis() / 1000UL);
    {
        JsonObject net = doc["network"].to<JsonObject>();
        net["ip"] = wifiConnectedAsClient ? WiFi.localIP().toString()
                                          : WiFi.softAPIP().toString();
    }

    // R19.D — tail of /reset_log.txt (last ≤16 lines)
    JsonArray rl = doc["resetLog"].to<JsonArray>();
    if (fsAvailable && activeFS && fsMutex) {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(1000));
        if (g.isLocked() && activeFS->exists("/reset_log.txt")) {
            File f = activeFS->open("/reset_log.txt", FILE_READ);
            if (f && f.size() <= 8 * 1024) {
                String buf = f.readString();
                f.close();
                // Tail: keep only the last 16 lines
                int newlines = 0;
                for (int i = (int)buf.length() - 1; i >= 0; i--) {
                    if (buf[i] == '\n') newlines++;
                    if (newlines > 16) { buf = buf.substring(i + 1); break; }
                }
                int start = 0;
                for (int i = 0; i < (int)buf.length(); i++) {
                    if (buf[i] == '\n') {
                        String line = buf.substring(start, i);
                        line.trim();
                        if (line.length() > 0) rl.add(line);
                        start = i + 1;
                    }
                }
                if (start < (int)buf.length()) {
                    String line = buf.substring(start);
                    line.trim();
                    if (line.length() > 0) rl.add(line);
                }
            } else if (f) {
                f.close();
            }
        }
    }

    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// GET /api/sensors/read_now?id=<sensorId>
//   Immediately reads a single non-blocking sensor and returns the values.
//   Blocking sensors (UART / HC-SR04 / Wind) are rejected with 400.
//   Uses wireMutex to avoid bus conflicts with the SensorTask.
// ---------------------------------------------------------------------------
static void handleApiSensorReadNow(AsyncWebServerRequest* req) {
    if (!req->hasArg("id")) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"missing id param\"}");
        return;
    }
    String id = req->arg("id");
    ISensor* s = sensorManager.getById(id.c_str());
    if (!s) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"sensor not found\"}");
        return;
    }
    if (!s->isEnabled()) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"sensor is disabled\"}");
        return;
    }
    if (s->isBlocking()) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"blocking sensor — use scheduled reads\"}");
        return;
    }

    SensorReading readings[8];
    bool tookMutex = false;
    if (wireMutex) {
        tookMutex = (xSemaphoreTake(wireMutex, pdMS_TO_TICKS(300)) == pdTRUE);
    }
    int n = s->readAll(readings, 8);
    if (tookMutex) xSemaphoreGive(wireMutex);

    if (n <= 0) {
        s->incErrorCount();
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"read failed\"}");
        return;
    }

    JsonDocument doc;
    doc["id"]   = s->getId();
    doc["type"] = s->getType();
    JsonArray arr = doc["readings"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject r = arr.add<JsonObject>();
        r["metric"] = readings[i].metric;
        r["value"]  = readings[i].value;
        r["unit"]   = readings[i].unit;
    }
    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/mqtt/ha_discovery — trigger HA MQTT discovery payloads on demand
// ---------------------------------------------------------------------------
static void handleMqttHaDiscovery(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF
#ifdef EXPORT_MQTT_ENABLED
    if (!g_mqttExporter) {
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"mqtt not initialised\"}");
        return;
    }
    g_mqttExporter->publishHaDiscovery();
    req->send(200, "application/json", "{\"ok\":true}");
#else
    req->send(404, "application/json", "{\"ok\":false,\"error\":\"mqtt not compiled\"}");
#endif
}

// ---------------------------------------------------------------------------
// GET /api/ota/status — OTA rollback status
// ---------------------------------------------------------------------------
static void handleOtaStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["running_partition"]  = OtaManager::runningPartitionLabel();
    doc["previous_partition"] = OtaManager::previousPartitionLabel();
    doc["pending_verify"]     = OtaManager::isPendingVerify();
    doc["rollback_capable"]   = OtaManager::isRollbackCapable();
    // Pass 5 5.6 — countdown until the rollback watchdog auto-confirms.
    // Zero when not pending or already confirmed; lets the UI surface a
    // "Confirming in N s" banner on the Update page.
    doc["confirm_in_ms"]      = OtaManager::millisUntilConfirm();
    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/ota/confirm — confirm current firmware as stable
// ---------------------------------------------------------------------------
static void handleOtaConfirm(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF
    if (OtaManager::confirm()) {
        req->send(200, "application/json", "{\"ok\":true}");
    } else {
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"confirm failed\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/ota/rollback — revert to previous firmware partition and restart
// ---------------------------------------------------------------------------
static void handleOtaRollback(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF
    req->send(200, "application/json",
              "{\"ok\":true,\"message\":\"Rolling back and restarting...\"}");
    // Set a flag consumed by loop() — avoids delay(200) blocking the AsyncTCP
    // worker while waiting for the response to be transmitted.  (AUDIT 3.16)
    g_pendingOtaRollback.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Pass 5 phase 3: generic /api/modules* endpoints.
//
// Coexists with the legacy /save_* handlers — both paths write to the same
// DeviceConfig, so either one staying the authoritative source of truth is
// fine during the transition.  Legacy save_* are kept through §5.8 step 5.
// ---------------------------------------------------------------------------

// GET /api/modules → [{id,name,enabled,hasUI}, ...]
static void handleApiModulesIndex(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    moduleRegistry.toIndexJson(arr);
    sendJsonResponse(req, doc);
}

// GET /api/modules/:id → {id,name,enabled,hasUI,config,schema?}
// The :id is extracted from the URL by the dispatcher below.
static void handleApiModuleDetail(AsyncWebServerRequest* req, const String& id) {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    if (!moduleRegistry.toDetailJson(id.c_str(), obj)) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown module\"}");
        return;
    }
    sendJsonResponse(req, doc);
}

// POST /api/modules/:id with JSON body → load() + persist.
// Uses an AsyncCallbackJsonWebHandler-style manual body buffer because the
// project already parses JSON bodies this way elsewhere.
static void handleApiModuleUpdate(AsyncWebServerRequest* req, const String& id,
                                   uint8_t* data, size_t len) {
    if (!requireMutatingAuth(req)) return;
    // TODO: JSON-body CSRF — CsrfToken::require() only reads form/query
    // params, so JSON callers must include ?csrf=... in the query string
    // until X-CSRF-Token header support lands.
    IModule* mod = moduleRegistry.getById(id.c_str());
    if (!mod) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown module\"}");
        return;
    }
    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);
    if (err) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    // Top-level "enabled" toggles runtime state; the rest of the payload is
    // the module's own field bag (schema shape).
    if (body["enabled"].is<bool>()) mod->setEnabled(body["enabled"].as<bool>());
    JsonObjectConst cfg = body["config"].is<JsonObjectConst>()
                          ? body["config"].as<JsonObjectConst>()
                          : body.as<JsonObjectConst>();
    if (!mod->load(cfg)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"validation failed\"}");
        return;
    }
    // Persist: saveConfig() already shadows modules.json via moduleRegistry.
    saveConfig();
    req->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/modules/:id/enable?on=1  — fast enable/disable without requiring
// the full config body.  Modules that cannot hot-restart still honour the
// flag; the next saveConfig() persists it and the caller can reboot via
// /restart if needed (audit Pass 5 5.3 "enable endpoint").
static void handleApiModuleEnable(AsyncWebServerRequest* req, const String& id) {
    if (!requireMutatingAuth(req)) return;   // was rate-limit only — add CSRF
    IModule* mod = moduleRegistry.getById(id.c_str());
    if (!mod) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown module\"}");
        return;
    }
    bool on = true;
    if (req->hasParam("on", true)) on = req->getParam("on", true)->value() == "1";
    else if (req->hasParam("on")) on = req->getParam("on")->value() == "1";
    mod->setEnabled(on);

    // Try a hot (re)start first; modules that cannot hot-cycle return false
    // from start() and the caller gets restartRequired=true in the reply.
    bool restartRequired = false;
    if (on) {
        if (!mod->start()) restartRequired = true;
    } else {
        mod->stop();
    }
    saveConfig();

    JsonDocument outDoc;
    outDoc["ok"] = true;
    outDoc["enabled"] = on;
    outDoc["restartRequired"] = restartRequired;
    sendJsonResponse(req, outDoc);
}

// ---------------------------------------------------------------------------
// R20 — POST /api/modules/:id/restart
// Calls module.stop() then module.start() without changing the enabled flag.
// Modules whose start() returns false (e.g. WiFi: can't bring the radio
// down on the AsyncTCP worker) report restartRequired=true so the caller
// knows to POST /restart for a device reboot.
// ---------------------------------------------------------------------------
static void handleApiModuleRestart(AsyncWebServerRequest* req, const String& id) {
    if (!requireMutatingAuth(req)) return;
    IModule* mod = moduleRegistry.getById(id.c_str());
    if (!mod) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown module\"}");
        return;
    }

    bool restartRequired = false;
    if (mod->isEnabled()) {
        mod->stop();
        if (!mod->start()) restartRequired = true;
    } else {
        // Disabled modules: still allow the call but report nothing happened.
        restartRequired = false;
    }

    JsonDocument outDoc;
    outDoc["ok"]              = true;
    outDoc["id"]              = mod->getId();
    outDoc["restartRequired"] = restartRequired;
    sendJsonResponse(req, outDoc);
}

// ---------------------------------------------------------------------------
// WiFi scan + credential-test endpoints (Pass 5 5.5 phase 1).
//
// Both endpoints are fully async-safe — no call inside an AsyncWebServer
// handler blocks the AsyncTCP task.  Strategy:
//   • scan: WiFi.scanNetworks(async=true) → GET polls via WiFi.scanComplete()
//   • test: a short-lived FreeRTOS task runs the connect-and-wait loop;
//           handlers only touch a file-static state machine (g_wtState).
// Both responses stream via AsyncResponseStream to avoid building a big
// contiguous String on the heap.
// ---------------------------------------------------------------------------

// ── WiFi test state machine ─────────────────────────────────────────────────
// g_wtState written from wifiTestTaskFn (separate FreeRTOS task, potentially
// on the other core) and read/consumed from AsyncTCP worker — requires
// release/acquire ordering so ip/rssi writes are visible to the reader before
// it observes WT_SUCCESS.  (AUDIT 3.11)
enum WifiTestState : uint8_t {
    WT_IDLE = 0, WT_RUNNING, WT_SUCCESS, WT_FAILED
};
static std::atomic<WifiTestState> g_wtState{WT_IDLE};
static char     g_wtSsid[33]        = "";
static char     g_wtPassword[65]    = "";
static int32_t  g_wtRssi            = 0;
static char     g_wtIp[20]          = "";
static char     g_wtError[48]       = "";

static void wifiTestTaskFn(void* /*arg*/) {
    // Save mode so a bad password can't drop a user who's connected over the
    // serving AP.  AP_STA keeps the AP alive during the probe.
    WiFiMode_t priorMode = WiFi.getMode();
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(g_wtSsid, g_wtPassword);

    constexpr uint32_t TIMEOUT_MS = 8000;
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_wtRssi = WiFi.RSSI();
        strlcpy(g_wtIp, WiFi.localIP().toString().c_str(), sizeof(g_wtIp));
        // release-store: ip/rssi writes above are visible to any thread that
        // observes WT_SUCCESS via an acquire-load.
        g_wtState.store(WT_SUCCESS, std::memory_order_release);
    } else {
        strlcpy(g_wtError, "timeout or auth failure", sizeof(g_wtError));
        g_wtState.store(WT_FAILED, std::memory_order_release);
    }

    // Tear down the probe connection but keep stored NVS creds intact —
    // `eraseap=true` would wipe the user's real saved network on a failed
    // test, which is decidedly not what they signed up for.
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
    WiFi.mode(priorMode);
    vTaskDelete(nullptr);
}

// GET /api/modules/wifi/scan  — starts an async scan on first call, returns
// the cached results on subsequent calls.  Never blocks more than a handful
// of microseconds.  Response shape:
//   • scan in progress: {ok, scanning:true}
//   • results ready:    {ok, scanning:false, count, networks:[…]}
static void handleApiWifiScan(AsyncWebServerRequest* req) {
    if (rateLimit429(req)) return;

    int n = WiFi.scanComplete();
    JsonDocument doc;

    if (n == WIFI_SCAN_RUNNING) {
        doc["ok"] = true;
        doc["scanning"] = true;
    } else if (n == WIFI_SCAN_FAILED || n < 0) {
        // No scan pending → kick a fresh one off.  ESP32 transparently goes
        // AP_STA for the duration so the serving AP stays up.
        WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
        doc["ok"] = true;
        doc["scanning"] = true;
    } else {
        doc["ok"] = true;
        doc["scanning"] = false;
        doc["count"] = n;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n && i < 32; i++) {    // cap at 32 entries
            JsonObject o = arr.add<JsonObject>();
            o["ssid"]    = WiFi.SSID(i);
            o["rssi"]    = WiFi.RSSI(i);
            o["channel"] = WiFi.channel(i);
            o["secure"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        WiFi.scanDelete();   // free slots so the next GET triggers a new scan
    }

    sendJsonResponse(req, doc);
}

// POST /api/modules/wifi/test  — kicks off a credential probe in a worker
// task and returns immediately with 202.  Clients poll the same path with
// GET to retrieve the result.  Body: {"ssid":"...","password":"..."}.
static void handleApiWifiTest(AsyncWebServerRequest* req,
                              uint8_t* data, size_t len) {
    if (!requireMutatingAuth(req)) return;
    // TODO: JSON-body CSRF — CsrfToken::require() only reads form/query
    // params, so JSON callers must include ?csrf=... in the query string
    // until X-CSRF-Token header support lands.

    JsonDocument body;
    if (deserializeJson(body, data, len)) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    const char* ssid = body["ssid"] | "";
    const char* pw   = body["password"] | "";
    if (!*ssid) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"missing ssid\"}");
        return;
    }
    if (g_wtState.load(std::memory_order_relaxed) == WT_RUNNING) {
        req->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"test already running\"}");
        return;
    }

    strlcpy(g_wtSsid,     ssid, sizeof(g_wtSsid));
    strlcpy(g_wtPassword, pw,   sizeof(g_wtPassword));
    g_wtRssi  = 0;
    g_wtIp[0] = '\0';
    g_wtError[0] = '\0';
    // Task creation provides the barrier that makes ssid/pw visible to the
    // newly spawned task; relaxed store is sufficient here.
    g_wtState.store(WT_RUNNING, std::memory_order_relaxed);

    BaseType_t rc = xTaskCreate(wifiTestTaskFn, "wifiTest", 4096,
                                 nullptr, 1, nullptr);
    if (rc != pdPASS) {
        g_wtState.store(WT_IDLE, std::memory_order_relaxed);
        req->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"cannot spawn task\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"ok\":true,\"state\":\"running\"}");
}

// GET /api/modules/wifi/test  — poll current test state.  Returns one of
// {state:"idle"}, {state:"running"}, {state:"success",rssi,ip},
// {state:"failed",error}.  Consumes the result on read so repeated polls
// after success/failed return "idle" (client gets one shot).
static void handleApiWifiTestPoll(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"] = true;
    // acquire-load: synchronises-with the release-store in wifiTestTaskFn so
    // g_wtRssi / g_wtIp / g_wtError are visible when SUCCESS/FAILED is seen.
    switch (g_wtState.load(std::memory_order_acquire)) {
        case WT_RUNNING:
            doc["state"] = "running";
            break;
        case WT_SUCCESS:
            doc["state"] = "success";
            doc["rssi"]  = g_wtRssi;
            doc["ip"]    = g_wtIp;
            g_wtState.store(WT_IDLE, std::memory_order_relaxed);  // consume
            break;
        case WT_FAILED:
            doc["state"] = "failed";
            doc["error"] = g_wtError;
            g_wtState.store(WT_IDLE, std::memory_order_relaxed);  // consume
            break;
        case WT_IDLE:
        default:
            doc["state"] = "idle";
            break;
    }
    sendJsonResponse(req, doc);
}

// Dispatcher — ESPAsyncWebServer's on() does exact-match only, so we register
// a single handler at "/api/modules/" that parses the tail segment.
static void handleApiModulesDispatch(AsyncWebServerRequest* req) {
    // GET /api/modules/:id  (update path handled by body callback below)
    String url = req->url();
    const char* prefix = "/api/modules/";
    if (!url.startsWith(prefix)) {
        req->send(404, "application/json", "{\"ok\":false}");
        return;
    }
    String id = url.substring(strlen(prefix));
    // Strip trailing slash or query if present.
    int q = id.indexOf('?'); if (q >= 0) id.remove(q);
    if (id.endsWith("/")) id.remove(id.length() - 1);
    if (id.length() == 0) {                 // /api/modules/ with no id
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
        return;
    }
    if (req->method() == HTTP_GET) {
        handleApiModuleDetail(req, id);
        return;
    }
    // POST lands here only after the body callback has run; the body is
    // delivered via the onBody handler registered alongside this route.
    req->send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
}

// ---------------------------------------------------------------------------
// GET /api/backup — full-state JSON snapshot (Pass 5 5.7).
//
// Bundles the JSON-layer config files into a single download so users can
// archive a complete known-good state and restore it on a new device:
//   /config/modules.json  → backup.modules
//   /config/sensors.json  → backup.sensors
//   platform_config.json  → backup.platform
// plus a header section identifying the device + firmware + boot count.
//
// /export_settings continues to expose the binary core config; clients
// that need the full picture fetch both and merge.  Restore is intentionally
// a separate endpoint (not yet shipped) — backup is the safe-to-ship slice.
// ---------------------------------------------------------------------------
static void handleApiBackup(AsyncWebServerRequest* req) {
    // Serialise against background config writes — handleApiData uses the
    // same configMutex pattern, and saveConfig() / handleApiModuleUpdate
    // can race with us otherwise.  500 ms is plenty for a JSON read pass.
    if (!configMutex || xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
        return;
    }

    AsyncResponseStream* resp = req->beginResponseStream("application/json");

    // Suggest a sensible filename so curl -OJ / browser save-as gets it
    // right (e.g. "waterlogger-backup-c8df84c4ed68-42.json").
    String fname = "waterlogger-backup-";
    fname += config.deviceId[0] ? config.deviceId : "device";
    fname += "-";
    fname += String((unsigned)bootCount);
    fname += ".json";
    resp->addHeader("Content-Disposition",
                    String("attachment; filename=\"") + fname + "\"");

    JsonDocument doc;
    doc["version"] = 1;
    // Prefer wall-clock time when the RTC has been set; fall back to uptime
    // seconds when it hasn't (gemini review PR #51).  Restore code can tell
    // the two apart by checking time_valid.
    if (rtcValid) {
        time_t now = 0; time(&now);
        doc["created_at"] = (uint32_t)now;
        doc["time_valid"] = true;
    } else {
        doc["created_at"] = (uint32_t)(millis() / 1000UL);
        doc["time_valid"] = false;
    }

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["name"]       = config.deviceName[0] ? config.deviceName : "Water Logger";
    dev["id"]         = config.deviceId;
    dev["firmware"]   = getVersionString();
    dev["boot_count"] = bootCount;

    // Deserialize each shadow file directly into the parent doc to avoid
    // the temp-doc + deep-copy round-trip (gemini review PR #51).
    auto inhaleJsonFile = [](JsonObject parent, const char* key, const char* path) {
        if (!activeFS || !activeFS->exists(path)) return;
        File f = activeFS->open(path, FILE_READ);
        if (!f) return;
        // 16 KB cap — same as ExportManager / SensorManager input caps;
        // beyond that we'd risk OOM on the AsyncTCP worker.
        if (f.size() > 16 * 1024) { f.close(); return; }
        JsonVariant slot = parent[key].to<JsonVariant>();
        if (deserializeJson(slot, f) != DeserializationError::Ok) {
            parent.remove(key);
        }
        f.close();
    };

    // Each section is best-effort — a missing file just leaves the key off
    // the response.  Restore code (future) must cope with absent keys.
    // Acquire fsMutex around all three reads so StorageTask / saveConfig can't
    // write a file mid-read.  Lock ordering: configMutex (already held) →
    // fsMutex — consistent with saveConfig which takes only fsMutex.  (AUDIT 3.20)
    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        inhaleJsonFile(doc.as<JsonObject>(), "modules",  "/config/modules.json");
        inhaleJsonFile(doc.as<JsonObject>(), "sensors",  "/config/sensors.json");
        inhaleJsonFile(doc.as<JsonObject>(), "platform", "/platform_config.json");
        xSemaphoreGive(fsMutex);
    } else {
        // Best-effort on timeout — files may be mid-write but we still send
        // whatever was deserialized rather than returning 503.
        inhaleJsonFile(doc.as<JsonObject>(), "modules",  "/config/modules.json");
        inhaleJsonFile(doc.as<JsonObject>(), "sensors",  "/config/sensors.json");
        inhaleJsonFile(doc.as<JsonObject>(), "platform", "/platform_config.json");
    }

    serializeJson(doc, *resp);
    xSemaphoreGive(configMutex);
    req->send(resp);
}

// ---------------------------------------------------------------------------
// GET /api/alerts — returns current rules + in-RAM history as JSON
// ---------------------------------------------------------------------------
static void handleApiAlertsGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    alertEngine.toJson(doc);
    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/alerts — replace the whole rules document and persist it
// ---------------------------------------------------------------------------
static void handleApiAlertsSave(AsyncWebServerRequest* req,
                                uint8_t* data, size_t len) {
    if (!requireMutatingAuth(req)) return;
    // TODO: JSON-body CSRF — CsrfToken::require() only reads form/query
    // params, so JSON callers must include ?csrf=... in the query string
    // until X-CSRF-Token header support lands.
    if (!alertEngine.fromJson(data, len)) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid JSON or save failed\"}");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// POST /api/alerts/snooze — body: {"rule_id":"...","until_ts":1234567890}
// ---------------------------------------------------------------------------
static void handleApiAlertsSnooze(AsyncWebServerRequest* req,
                                  uint8_t* data, size_t len) {
    if (!requireMutatingAuth(req)) return;
    // TODO: JSON-body CSRF — CsrfToken::require() only reads form/query
    // params, so JSON callers must include ?csrf=... in the query string
    // until X-CSRF-Token header support lands.
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)data, len) != DeserializationError::Ok) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }
    const char* ruleId  = doc["rule_id"]  | "";
    uint32_t    until   = doc["until_ts"] | (uint32_t)0;

    if (!ruleId[0]) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"rule_id required\"}");
        return;
    }
    if (!alertEngine.snooze(ruleId, until)) {
        req->send(404, "application/json",
                  "{\"ok\":false,\"error\":\"rule not found\"}");
        return;
    }
    req->send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// GET /api/alerts/toasts — drain the pending toast notification queue
// ---------------------------------------------------------------------------
static void handleApiAlertsToasts(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    AlertToast toast;
    while (alertEngine.popToast(toast)) {
        JsonObject o = arr.add<JsonObject>();
        o["rule_id"] = toast.rule_id;
        o["name"]    = toast.name;
        o["value"]   = toast.value;
        o["ts"]      = toast.ts;
    }
    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/i2c_scan[?bus=N] — scan an I2C bus and return detected addresses
//
// `bus` defaults to 0. Only buses a sensor has already brought up can be
// scanned: the pins are a property of the sensor config, so there is nothing
// to scan on a bus nobody has claimed. Scanning an unconfigured bus would mean
// guessing pins and calling begin() behind the sensor task's back.
//
// The response stays a bare array of address strings for backwards
// compatibility with the existing UI.
// ---------------------------------------------------------------------------
static void handleApiI2cScan(AsyncWebServerRequest* req) {
    if (!requireMutatingAuth(req)) return;   // rate-limit + CSRF

    uint8_t bus = 0;
    if (req->hasParam("bus", true))      bus = (uint8_t)req->getParam("bus", true)->value().toInt();
    else if (req->hasParam("bus"))       bus = (uint8_t)req->getParam("bus")->value().toInt();

    // Safe to check without the lock: the controller count is a compile-time
    // property of the chip and never changes.
    if (bus >= I2CBus::hardwareBusCount()) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"bus not available on this chip\"}");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    // Acquire the I2C bus mutex if available (same guard used by sensors).
    // One mutex covers every bus — see I2CBus.h on why that is deliberate.
    extern SemaphoreHandle_t wireMutex;
    bool tookMutex = false;
    if (wireMutex) {
        tookMutex = (xSemaphoreTake(wireMutex, pdMS_TO_TICKS(500)) == pdTRUE);
    }
    if (!tookMutex) {
        req->send(503, "application/json",
                  "{\"ok\":false,\"error\":\"bus busy\"}");
        return;
    }

    // Resolve the handle only AFTER the lock. A config reload takes wireMutex
    // and calls I2CBus::resetAll(), which end()s the peripheral and frees the
    // driver's rx/tx buffers — so a pointer fetched before blocking on the
    // mutex could name a bus that was torn down while we waited.
    TwoWire* wire = I2CBus::get(bus);
    if (!wire) {
        xSemaphoreGive(wireMutex);
        req->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"bus not configured — assign an I2C sensor to it first\"}");
        return;
    }

    for (uint8_t addr = 0x01; addr <= 0x7F; addr++) {
        wire->beginTransmission(addr);
        if (wire->endTransmission() == 0) {
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", addr);
            arr.add(hex);
        }
    }
    xSemaphoreGive(wireMutex);

    sendJsonResponse(req, doc);
}


// ---------------------------------------------------------------------------
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/data",              HTTP_GET,  handleApiData);
    server.on("/api/latest",            HTTP_GET,  handleApiLatest);
    server.on("/api/sensors",           HTTP_GET,  handleApiSensors);
    server.on("/api/sensors/read_now",  HTTP_GET,  handleApiSensorReadNow);
    server.on("/api/diag",              HTTP_GET,  handleApiDiag);
    server.on("/api/backup",            HTTP_GET,  handleApiBackup);
    server.on("/api/config/platform",   HTTP_POST, handleConfigPlatform);
    server.on("/api/mqtt/ha_discovery", HTTP_POST, handleMqttHaDiscovery);
    server.on("/api/ota/status",        HTTP_GET,  handleOtaStatus);
    server.on("/api/ota/confirm",       HTTP_POST, handleOtaConfirm);
    server.on("/api/ota/rollback",      HTTP_POST, handleOtaRollback);

    // Pass 5 5.5 phase 1 — WiFi-specific helpers. Registered BEFORE the generic
    // per-module routes below: the fork prefix-matches, so the per-module GET
    // "/api/modules/wifi" would otherwise swallow "/api/modules/wifi/scan" and
    // "/api/modules/wifi/test". Only registered when the wifi module is present
    // so stripped-down builds don't pay the flash cost.
    if (moduleRegistry.getById("wifi")) {
        server.on("/api/modules/wifi/scan", HTTP_GET,  handleApiWifiScan);
        server.on("/api/modules/wifi/test", HTTP_GET,  handleApiWifiTestPoll);
        server.on("/api/modules/wifi/test", HTTP_POST,
            [](AsyncWebServerRequest* r) { /* auth + body handled in onBody below */ },
            nullptr,
            [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
               size_t index, size_t total) {
                if (index != 0 || len != total) {
                    r->send(413, "application/json",
                            "{\"ok\":false,\"error\":\"body too large\"}");
                    return;
                }
                handleApiWifiTest(r, data, len);
            });
    }

    // Pass 5 phase 3: generic module CRUD.
    //
    // IMPORTANT — route ordering vs. prefix matching:
    // The esphome ESPAsyncWebServer fork PREFIX-matches handlers
    // (canHandle ≈ `url == uri || url.startsWith(uri + "/")`), NOT exact-match
    // as an earlier comment here assumed. Handlers are tried in registration
    // order and the first whose canHandle() passes wins, so MORE-SPECIFIC paths
    // MUST be registered BEFORE shorter ones:
    //   • "/api/modules" registered first would swallow "/api/modules/<id>" —
    //     the detail/form GET would return the whole index array, so the UI
    //     shows "no configurable form".
    //   • "/api/modules/<id>" registered before "<id>/enable" / "<id>/restart"
    //     would swallow those POSTs.
    // Hence: per-module enable/restart first, then "/api/modules/<id>", and the
    // "/api/modules" index LAST.
    for (int i = 0; i < moduleRegistry.count(); i++) {
        String base = String("/api/modules/") + moduleRegistry.get(i)->getId();

        // POST /api/modules/:id/enable — flip a module without the full config
        // blob. Registered first so it isn't shadowed by /api/modules/:id POST.
        String enablePath = base + "/enable";
        server.on(enablePath.c_str(), HTTP_POST, [](AsyncWebServerRequest* r) {
            String url = r->url();
            String id = url.substring(strlen("/api/modules/"));
            // Strip query string before the suffix check — a URL like
            // /api/modules/wifi/enable?on=1 would otherwise leave the query in
            // `id` and break the endsWith.
            int q = id.indexOf('?');
            if (q >= 0) id.remove(q);
            if (id.endsWith("/enable")) id.remove(id.length() - strlen("/enable"));
            handleApiModuleEnable(r, id);
        });

        // R20 — POST /api/modules/:id/restart — stop() + start() without
        // changing the enabled flag. restartRequired=true means start() can't
        // hot-restart from a web handler (e.g. WiFi) — caller POSTs /restart.
        String restartPath = base + "/restart";
        server.on(restartPath.c_str(), HTTP_POST, [](AsyncWebServerRequest* r) {
            String url = r->url();
            String id = url.substring(strlen("/api/modules/"));
            int q = id.indexOf('?');
            if (q >= 0) id.remove(q);
            if (id.endsWith("/restart")) id.remove(id.length() - strlen("/restart"));
            handleApiModuleRestart(r, id);
        });

        // GET /api/modules/:id — detail object + PROGMEM schema (drives the form)
        server.on(base.c_str(), HTTP_GET, handleApiModulesDispatch);

        // POST /api/modules/:id — save {enabled, config}. Body buffered via
        // onBody. Registered AFTER enable/restart so it doesn't prefix-swallow
        // them. ESPAsyncWebServer copies the URL internally, so stack-local
        // Strings are fine here.
        server.on(base.c_str(), HTTP_POST,
            [](AsyncWebServerRequest* r) { /* auth + body handled in onBody below */ },
            nullptr,
            [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
               size_t index, size_t total) {
                // Small module payloads (≤ a few hundred bytes) arrive in a
                // single chunk on LittleFS-backed boards; reject chunked
                // uploads rather than buffer unbounded bytes in RAM.
                if (index != 0 || len != total) {
                    r->send(413, "application/json",
                            "{\"ok\":false,\"error\":\"body too large\"}");
                    return;
                }
                String url = r->url();
                String id  = url.substring(sizeof("/api/modules/") - 1);
                int q = id.indexOf('?'); if (q >= 0) id.remove(q);
                handleApiModuleUpdate(r, id, data, len);
            });
    }

    // Index LAST — the per-module GET routes above must be tried first so
    // "/api/modules/<id>" resolves to the detail handler, not this index.
    server.on("/api/modules", HTTP_GET, handleApiModulesIndex);

    // Alert engine endpoints (4.2 — new IoT features)
    server.on("/api/alerts",        HTTP_GET,  handleApiAlertsGet);
    server.on("/api/alerts/toasts", HTTP_GET,  handleApiAlertsToasts);
    server.on("/api/i2c_scan",      HTTP_POST, handleApiI2cScan);

    // POST /api/alerts — whole-document replace
    server.on("/api/alerts", HTTP_POST,
        [](AsyncWebServerRequest* r) { /* auth + body handled in onBody below */ },
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (index != 0 || len != total) {
                r->send(413, "application/json",
                        "{\"ok\":false,\"error\":\"body too large\"}");
                return;
            }
            handleApiAlertsSave(r, data, len);
        });

    // POST /api/alerts/snooze
    server.on("/api/alerts/snooze", HTTP_POST,
        [](AsyncWebServerRequest* r) { /* auth + body handled in onBody below */ },
        nullptr,
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (index != 0 || len != total) {
                r->send(413, "application/json",
                        "{\"ok\":false,\"error\":\"body too large\"}");
                return;
            }
            handleApiAlertsSnooze(r, data, len);
        });
}
