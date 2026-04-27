#include "ApiHandlers.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>                      // WiFi scan/test (Pass 5 5.5 phase 1)
#include <time.h>                      // /api/backup created_at (Pass 5 5.7)
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
    // Pass 5 5.6 — countdown until the rollback watchdog auto-confirms.
    // Zero when not pending or already confirmed; lets the UI surface a
    // "Confirming in N s" banner on the Update page.
    doc["confirm_in_ms"]      = OtaManager::millisUntilConfirm();
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
enum WifiTestState : uint8_t {
    WT_IDLE = 0, WT_RUNNING, WT_SUCCESS, WT_FAILED
};
static volatile WifiTestState g_wtState = WT_IDLE;
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
        g_wtState = WT_SUCCESS;
    } else {
        strlcpy(g_wtError, "timeout or auth failure", sizeof(g_wtError));
        g_wtState = WT_FAILED;
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

    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);
}

// POST /api/modules/wifi/test  — kicks off a credential probe in a worker
// task and returns immediately with 202.  Clients poll the same path with
// GET to retrieve the result.  Body: {"ssid":"...","password":"..."}.
static void handleApiWifiTest(AsyncWebServerRequest* req,
                              uint8_t* data, size_t len) {
    if (rateLimit429(req)) return;

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
    if (g_wtState == WT_RUNNING) {
        req->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"test already running\"}");
        return;
    }

    strlcpy(g_wtSsid,     ssid, sizeof(g_wtSsid));
    strlcpy(g_wtPassword, pw,   sizeof(g_wtPassword));
    g_wtRssi  = 0;
    g_wtIp[0] = '\0';
    g_wtError[0] = '\0';
    g_wtState = WT_RUNNING;

    BaseType_t rc = xTaskCreate(wifiTestTaskFn, "wifiTest", 4096,
                                 nullptr, 1, nullptr);
    if (rc != pdPASS) {
        g_wtState = WT_IDLE;
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
    switch (g_wtState) {
        case WT_RUNNING:
            doc["state"] = "running";
            break;
        case WT_SUCCESS:
            doc["state"] = "success";
            doc["rssi"]  = g_wtRssi;
            doc["ip"]    = g_wtIp;
            g_wtState = WT_IDLE;    // consume
            break;
        case WT_FAILED:
            doc["state"] = "failed";
            doc["error"] = g_wtError;
            g_wtState = WT_IDLE;    // consume
            break;
        case WT_IDLE:
        default:
            doc["state"] = "idle";
            break;
    }
    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);
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
    inhaleJsonFile(doc.as<JsonObject>(), "modules",  "/config/modules.json");
    inhaleJsonFile(doc.as<JsonObject>(), "sensors",  "/config/sensors.json");
    inhaleJsonFile(doc.as<JsonObject>(), "platform", "/platform_config.json");

    serializeJson(doc, *resp);
    xSemaphoreGive(configMutex);
    req->send(resp);
}

// ---------------------------------------------------------------------------
void registerApiRoutes(AsyncWebServer& server) {
    server.on("/api/data",              HTTP_GET,  handleApiData);
    server.on("/api/sensors",           HTTP_GET,  handleApiSensors);
    server.on("/api/sensors/read_now",  HTTP_GET,  handleApiSensorReadNow);
    server.on("/api/diag",              HTTP_GET,  handleApiDiag);
    server.on("/api/backup",            HTTP_GET,  handleApiBackup);
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

    // Pass 5 5.5 phase 1 — WiFi-specific helpers.  Registered only when the
    // wifi module is present so stripped-down builds don't pay the flash cost.
    // All three endpoints are fully async-safe (see handler comments).
    if (moduleRegistry.getById("wifi")) {
        server.on("/api/modules/wifi/scan", HTTP_GET,  handleApiWifiScan);
        server.on("/api/modules/wifi/test", HTTP_GET,  handleApiWifiTestPoll);
        server.on("/api/modules/wifi/test", HTTP_POST,
            [](AsyncWebServerRequest* r) { /* body handled below */ },
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
}
