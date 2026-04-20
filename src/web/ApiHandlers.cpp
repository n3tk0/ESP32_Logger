#include "ApiHandlers.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/task.h>
#include "../pipeline/DataPipeline.h"
#include "../pipeline/AggregationEngine.h"
#include "../sensors/SensorManager.h"
#include "../export/ExportManager.h"
#include "../export/MqttExporter.h"
#include "../storage/JsonLogger.h"
#include "../core/Globals.h"         // config, activeFS
#include "../core/ModuleRegistry.h"  // Pass 5 phase 3: /api/modules
#include "../managers/ConfigManager.h" // saveConfig() after module update
#include "RateLimiter.h"               // Pass 7 rate-limit on mutating routes

// Forward-declared in Logger.ino — accessible here because this file is
// compiled in the same sketch scope.
#ifdef EXPORT_MQTT_ENABLED
extern MqttExporter* g_mqttExporter;
#endif
#include "../tasks/TaskManager.h"  // task handles for /api/diag
#include "../managers/OtaManager.h"

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
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
        return;
    }

    // Reserve up to 200 slots for the ring buffer; give all remaining slots to
    // the filesystem query.  When the ring is empty (historical request) the
    // full MAX_RAW budget is available for FS rows instead of a fixed 300-cap.
    constexpr size_t RING_SHARE = 200;

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

    SensorReading* agg     = new SensorReading[limit + 1];
    size_t         aggCount = 0;
    bool           truncated = false;

    if (!agg) {
        delete[] raw;
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
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
                size_t fsShare = MAX_RAW - ringCount;  // full budget when ring is empty
                fsCount = logger.query(*activeFS, fromTs, toTs,
                                       sensorFilter, metricFilter,
                                       raw + ringCount, fsShare);
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
    JsonDocument doc;
    JsonArray arr = doc["sensors"].to<JsonArray>();
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
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
        return;
    }
    // Lock config mutex so tasks don't read a partially-updated config
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        bool sensorsOk   = sensorManager.reloadConfig(*activeFS);
        bool exportersOk = exportManager.reloadConfig(*activeFS);
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

    // Heap
    doc["free_heap"]     = (uint32_t)ESP.getFreeHeap();
    doc["min_free_heap"] = (uint32_t)ESP.getMinFreeHeap();
    doc["queue_drops"]   = (uint32_t)g_queueDrops;

    // Queues
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

    // OTA rollback info
    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["running"]          = OtaManager::runningPartitionLabel();
    ota["previous"]         = OtaManager::previousPartitionLabel();
    ota["pending_verify"]   = OtaManager::isPendingVerify();
    ota["rollback_capable"] = OtaManager::isRollbackCapable();

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
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
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// POST /api/mqtt/ha_discovery — trigger HA MQTT discovery payloads on demand
// ---------------------------------------------------------------------------
static void handleMqttHaDiscovery(AsyncWebServerRequest* req) {
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
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// POST /api/ota/confirm — confirm current firmware as stable
// ---------------------------------------------------------------------------
static void handleOtaConfirm(AsyncWebServerRequest* req) {
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
    req->send(200, "application/json",
              "{\"ok\":true,\"message\":\"Rolling back and restarting...\"}");
    // Delay rollback so the response is sent first
    shouldRestart = false;  // prevent normal restart path
    delay(200);
    OtaManager::rollback();
    // If rollback() returns (shouldn't normally), fall back to restart
    shouldRestart = true;
    restartTimer  = millis();
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
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
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
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// POST /api/modules/:id with JSON body → load() + persist.
// Uses an AsyncCallbackJsonWebHandler-style manual body buffer because the
// project already parses JSON bodies this way elsewhere.
static void handleApiModuleUpdate(AsyncWebServerRequest* req, const String& id,
                                   uint8_t* data, size_t len) {
    if (rateLimit429(req)) return;
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
    if (rateLimit429(req)) return;
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

    JsonDocument out;
    out["ok"] = true;
    out["enabled"] = on;
    out["restartRequired"] = restartRequired;
    String body;
    serializeJson(out, body);
    req->send(200, "application/json", body);
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
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/data",              HTTP_GET,  handleApiData);
    server.on("/api/sensors",           HTTP_GET,  handleApiSensors);
    server.on("/api/sensors/read_now",  HTTP_GET,  handleApiSensorReadNow);
    server.on("/api/diag",              HTTP_GET,  handleApiDiag);
    server.on("/api/config/platform",   HTTP_POST, handleConfigPlatform);
    server.on("/api/mqtt/ha_discovery", HTTP_POST, handleMqttHaDiscovery);
    server.on("/api/ota/status",        HTTP_GET,  handleOtaStatus);
    server.on("/api/ota/confirm",       HTTP_POST, handleOtaConfirm);
    server.on("/api/ota/rollback",      HTTP_POST, handleOtaRollback);

    // Pass 5 phase 3: generic module CRUD
    server.on("/api/modules", HTTP_GET, handleApiModulesIndex);

    // Per-module routes (ESPAsyncWebServer does exact-match only; iterating is
    // cheap with MAX_MODULES = 16).  POST uses onBody so the full JSON payload
    // is buffered before the dispatcher runs.  ESPAsyncWebServer copies the
    // URL string internally, so a stack-local String is fine here.
    for (int i = 0; i < moduleRegistry.count(); i++) {
        String base = String("/api/modules/") + moduleRegistry.get(i)->getId();
        server.on(base.c_str(), HTTP_GET, handleApiModulesDispatch);
        server.on(base.c_str(), HTTP_POST,
            [](AsyncWebServerRequest* r) { /* body handled below */ },
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

        // Dedicated enable/disable endpoint — lets the UI flip a module
        // without sending the whole config blob.
        String enablePath = base + "/enable";
        server.on(enablePath.c_str(), HTTP_POST, [](AsyncWebServerRequest* r) {
            String url = r->url();
            const char* prefix = "/api/modules/";
            String id = url.substring(strlen(prefix));
            if (id.endsWith("/enable")) id.remove(id.length() - strlen("/enable"));
            handleApiModuleEnable(r, id);
        });
    }
}
