#include "TaskManager.h"
#include "SensorTask.h"
#include "SlowSensorTask.h"
#include "ProcessingTask.h"
#include "StorageTask.h"
#include "ExportTask.h"
#include "../pipeline/DataPipeline.h"
#include "../sensors/SensorManager.h"  // sensorManager.count() for dynamic queue sizing
#include "../core/Globals.h"            // sdAvailable, littleFsAvailable, activeFS
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>

// Static member definitions
TaskHandle_t      TaskManager::hSensor     = nullptr;
TaskHandle_t      TaskManager::hSlowSensor = nullptr;
TaskHandle_t      TaskManager::hProcess    = nullptr;
TaskHandle_t      TaskManager::hStorage    = nullptr;
TaskHandle_t      TaskManager::hExport     = nullptr;
std::atomic<bool> TaskManager::running(false);
std::atomic<bool> TaskManager::startGate(false);

// ---------------------------------------------------------------------------
// waitForStart() — block a freshly-created pipeline task until init() has
// finished building every queue/mutex and opened the gate. Without this the
// higher-priority sensor/process tasks (prio 2-3 vs loop's prio 1) preempt
// init() the moment they are created, observe running==false, and self-delete.
// Returns false if shutdown() ran before the gate opened (task should exit).
// ---------------------------------------------------------------------------
bool TaskManager::waitForStart() {
    // Cap the wait so a never-opened gate (e.g. init aborted between this
    // task's creation and opening the gate) can't wedge a task forever. On the
    // normal path init() opens the gate within microseconds. On the failure
    // path _cleanupPartialInit() force-deletes these handles, so this loop is
    // only a safety net.
    for (int i = 0; i < 1000; i++) {            // up to ~10 s
        if (startGate.load(std::memory_order_acquire)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return startGate.load(std::memory_order_acquire) && running.load();
}

// Storage task needs a persistent param (lives for task lifetime)
static StorageTaskParam storageParam;

// Mirror storage param (dual-write when SD + LittleFS both available)
static StorageTaskParam mirrorParam;

// Persistent storage for logDir string (must outlive storageParam)
static char s_logDir[48] = "/logs";

// ---------------------------------------------------------------------------
// R12: Clean up partially-built task/queue state so a failed init() doesn't
// leak FreeRTOS objects.  Idempotent.
//
// IMPORTANT — does NOT delete mutexes.  Per AUDIT 1.6 the mutexes must
// SURVIVE an init failure: the caller trips g_safeMode and lets the web
// stack continue, where ApiHandlers may xSemaphoreTake(fsMutex) etc.
// Tearing them down here would re-introduce the NULL-mutex crash 1.6
// originally addressed.  Mutexes are cheap and persistent.
//
// AUDIT 2.2 — was previously leaking queues/tasks on every early return
// and leaving running=true on a half-built world.
static void _cleanupPartialInit() {
    // Tasks first — handles must be deleted before the queues they read.
    if (TaskManager::hSensor)     { vTaskDelete(TaskManager::hSensor);     TaskManager::hSensor     = nullptr; }
    if (TaskManager::hSlowSensor) { vTaskDelete(TaskManager::hSlowSensor); TaskManager::hSlowSensor = nullptr; }
    if (TaskManager::hProcess)    { vTaskDelete(TaskManager::hProcess);    TaskManager::hProcess    = nullptr; }
    if (TaskManager::hStorage)    { vTaskDelete(TaskManager::hStorage);    TaskManager::hStorage    = nullptr; }
    if (TaskManager::hExport)     { vTaskDelete(TaskManager::hExport);     TaskManager::hExport     = nullptr; }
    if (sensorQueue)  { vQueueDelete(sensorQueue);  sensorQueue  = nullptr; }
    if (storageQueue) { vQueueDelete(storageQueue); storageQueue = nullptr; }
    if (exportQueue)  { vQueueDelete(exportQueue);  exportQueue  = nullptr; }
    // Mutexes intentionally left alive — see comment above.
    TaskManager::running   = false;
    TaskManager::startGate = false;  // never leave the gate open on a failed init
}

// PR #105 follow-up: factored out so /api/config/platform can re-apply SDS011
// humidity correction live.  StorageTask polls *p (= storageParam) every
// aggregation tick (see StorageTask.cpp:87-94) and re-arms its LiveAggregator
// from p->humidityCorrection*, so mutating storageParam here is enough — no
// task restart required.  Caller must hold configMutex.
void TaskManager::refreshStorageFromPlatform(fs::FS& fs) {
    storageParam.humidityCorrectionEnabled = false;
    storageParam.humidityCorrectionKappa   = 0.35f;   // codebase default

    File cfgFile = fs.open("/platform_config.json", FILE_READ);
    if (!cfgFile) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, cfgFile);
    cfgFile.close();
    if (err) return;

    JsonArrayConst sensors = doc["sensors"].as<JsonArrayConst>();
    for (JsonObjectConst sensor : sensors) {
        if (strcmp(sensor["type"] | "", "sds011") == 0) {
            storageParam.humidityCorrectionEnabled =
                sensor["humidityCorrectionEnabled"] | false;
            float k = sensor["humidityCorrectionKappa"] | 0.35f;
            storageParam.humidityCorrectionKappa = (k > 0.0f) ? k : 0.35f;
            break;
        }
    }
}

bool TaskManager::init(fs::FS& fs) {
    // AUDIT 2.3: do NOT set running=true here. Tasks + queues + mutexes are
    // built below; if any step fails, half-built state would have left
    // running=true with NULL queues, racing the rest of the system.
    // running is set ONLY at the bottom, just before return true.

    // Keep the start gate closed until every queue/mutex/task is built. Tasks
    // created below block in waitForStart() until we open it, so a high-prio
    // task can't preempt us, see running==false, and self-delete. (AUDIT 2.3
    // follow-up — see waitForStart()/startGate.)
    startGate = false;

    // Zero heartbeats — stale values survive warm reboots (RTC_SW_CPU_RST)
    // and cause immediate false-positive watchdog triggers (#C4).
    for (int i = 0; i < TASK_COUNT; i++) g_taskHeartbeat[i] = millis();

    // ── Mutexes FIRST ────────────────────────────────────────────────────
    // AUDIT 1.6: mutex creation precedes everything else so even a later
    // queue/task failure still leaves a valid synchronisation surface for
    // ApiHandlers / WebServer that may try to xSemaphoreTake(fsMutex) etc.
    webDataMutex = xSemaphoreCreateMutex();
    configMutex  = xSemaphoreCreateMutex();
    wireMutex    = xSemaphoreCreateMutex();   // I2C bus serialisation (#14)
    fsMutex      = xSemaphoreCreateMutex();   // FS write serialisation (FS1)

    if (!webDataMutex || !configMutex || !wireMutex || !fsMutex) {
        Serial.println("[TaskManager] Mutex creation FAILED");
        _cleanupPartialInit();
        return false;
    }

    // ── Queues ───────────────────────────────────────────────────────────
    // Dynamic queue depth: scale sensor queue to actual sensor count × 4 metrics
    // so one full tick cycle never drops readings (#3.5)
    int sCount    = sensorManager.count();
    int dynSDepth = (sCount > 0) ? max((int)QUEUE_SENSOR_DEPTH, sCount * 4)
                                 : (int)QUEUE_SENSOR_DEPTH;

    sensorQueue  = xQueueCreate((UBaseType_t)dynSDepth, sizeof(SensorReading));
    storageQueue = xQueueCreate(QUEUE_STORAGE_DEPTH,    sizeof(SensorReading));
    exportQueue  = xQueueCreate(QUEUE_EXPORT_DEPTH,     sizeof(SensorReading));
    Serial.printf("[TaskManager] sensorQueue depth=%d (sensors=%d)\n",
                  dynSDepth, sCount);

    if (!sensorQueue || !storageQueue || !exportQueue) {
        Serial.println("[TaskManager] Queue creation FAILED");
        _cleanupPartialInit();
        return false;
    }

    // Parse storage config from platform_config.json (#8).  Storage knobs
    // (log_dir / max_size_kb / rotate_daily) are read-once at boot; SDS011
    // humidity correction is refactored into refreshStorageFromPlatform()
    // so /api/config/platform can re-apply it live.
    {
        File cfgFile = fs.open("/platform_config.json", FILE_READ);
        if (cfgFile) {
            JsonDocument doc;
            if (deserializeJson(doc, cfgFile) == DeserializationError::Ok) {
                JsonObjectConst st = doc["storage"];
                if (!st.isNull()) {
                    const char* dir = st["log_dir"] | "/logs";
                    strncpy(s_logDir, dir, sizeof(s_logDir) - 1);
                    storageParam.maxSizeKB   = st["max_size_kb"]   | 512;
                    storageParam.rotateDaily = st["rotate_daily"]  | true;
                }
            }
            cfgFile.close();
        }
    }
    refreshStorageFromPlatform(fs);
    storageParam.fs     = &fs;
    storageParam.logDir = s_logDir;

    // Wide-CSV pipeline knobs sourced from DeviceConfig (set via web UI).
    storageParam.csvLoggingEnabled         = config.logger.csvLoggingEnabled;
    storageParam.aggregationIntervalSec    = config.logger.aggregationIntervalSec
                                                ? config.logger.aggregationIntervalSec : 60;

    // FlowRunLogger: per-fill flowmeter logging.  Active in PLATFORM_HYBRID
    // only — PLATFORM_LEGACY uses DataLogger.cpp's run logger and
    // PLATFORM_CONTINUOUS streams flow readings through the wide-CSV
    // pipeline.  Compile-time gated by SENSOR_WATERFLOW_ENABLED so non-
    // flowmeter builds DCE the class entirely.
#if defined(SENSOR_WATERFLOW_ENABLED)
    storageParam.enableFlowRunLogger = (g_platformMode == PLATFORM_HYBRID);
#else
    storageParam.enableFlowRunLogger = false;
#endif

    // Mirror write: if SD is primary and LittleFS is also available (or vice versa),
    // and config requests "mirror" mode, start a second StorageTask on the other FS.
    storageParam.mirrorFS = nullptr;
    {
        File cfgFile2 = fs.open("/platform_config.json", FILE_READ);
        if (cfgFile2) {
            JsonDocument doc2;
            if (deserializeJson(doc2, cfgFile2) == DeserializationError::Ok) {
                const char* stMode = doc2["storage"]["mode"] | "primary";
                if (strcmp(stMode, "mirror") == 0 && sdAvailable && littleFsAvailable) {
                    // Primary is SD → mirror is LittleFS, or vice versa
                    storageParam.mirrorFS = (&fs == &SD)
                                           ? static_cast<fs::FS*>(&LittleFS)
                                           : static_cast<fs::FS*>(&SD);
                    Serial.println("[TaskManager] Mirror write enabled (SD + LittleFS)");
                }
            }
            cfgFile2.close();
        }
    }

    // ── Tasks ────────────────────────────────────────────────────────────
    // AUDIT 2.4: consumers (StorageTask, ExportTask) MUST exist before any
    // producer enqueues data — otherwise the storage / export queues fill
    // to depth and producers drop readings until the consumers come up.
    // Order: storage → export → fast sensors → slow sensors → process.
    BaseType_t r;

    r = xTaskCreatePinnedToCore(storageTaskFunc,    "StorageTask",
                                STACK_STORAGE_TASK, &storageParam,
                                TASK_PRIO_STORAGE,  &hStorage,  0);
    if (r != pdPASS) { Serial.println("[TaskManager] StorageTask FAILED"); _cleanupPartialInit(); return false; }

    r = xTaskCreatePinnedToCore(exportTaskFunc,     "ExportTask",
                                STACK_EXPORT_TASK,  nullptr,
                                TASK_PRIO_EXPORT,   &hExport,   0);
    if (r != pdPASS) { Serial.println("[TaskManager] ExportTask FAILED"); _cleanupPartialInit(); return false; }

    r = xTaskCreatePinnedToCore(sensorTaskFunc,     "SensorTask",
                                STACK_SENSOR_TASK,  nullptr,
                                TASK_PRIO_SENSOR,   &hSensor,   0);
    if (r != pdPASS) { Serial.println("[TaskManager] SensorTask FAILED"); _cleanupPartialInit(); return false; }

    r = xTaskCreatePinnedToCore(slowSensorTaskFunc,     "SlowSensorTask",
                                STACK_SLOW_SENSOR_TASK, nullptr,
                                TASK_PRIO_SLOW_SENSOR,  &hSlowSensor, 0);
    if (r != pdPASS) { Serial.println("[TaskManager] SlowSensorTask FAILED"); _cleanupPartialInit(); return false; }

    r = xTaskCreatePinnedToCore(processingTaskFunc, "ProcessTask",
                                STACK_PROCESS_TASK, nullptr,
                                TASK_PRIO_PROCESS,  &hProcess,  0);
    if (r != pdPASS) { Serial.println("[TaskManager] ProcessTask FAILED"); _cleanupPartialInit(); return false; }

    Serial.println("[TaskManager] All tasks started");

    // AUDIT 2.3: running=true ONLY now, after every resource is built.
    running = true;
    // Re-seed heartbeats to "now" so the up-to-10s any task may have spent
    // parked in waitForStart() doesn't count against the 30s silence budget.
    for (int i = 0; i < TASK_COUNT; i++) g_taskHeartbeat[i] = millis();
    // Open the gate LAST: tasks parked in waitForStart() now observe both
    // running==true and startGate==true and enter their work loops.
    startGate.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
void TaskManager::shutdown() {
    running.store(false, std::memory_order_release);
    // Release any task still parked at startup so it observes running==false
    // and exits cleanly instead of waiting out the full waitForStart() timeout.
    startGate.store(true, std::memory_order_release);

    // Wait for sensor queues to drain (up to 3s) before hard timeout.
    // Prevents storageQueue data loss when sensor pipeline is still writing.
    constexpr uint32_t DRAIN_TIMEOUT_MS = 3000;
    uint32_t deadline = millis() + DRAIN_TIMEOUT_MS;
    while (millis() < deadline) {
        UBaseType_t sq = sensorQueue  ? uxQueueMessagesWaiting(sensorQueue)  : 0;
        UBaseType_t stq = storageQueue ? uxQueueMessagesWaiting(storageQueue) : 0;
        UBaseType_t eq = exportQueue  ? uxQueueMessagesWaiting(exportQueue)  : 0;
        if (sq == 0 && stq == 0 && eq == 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // AUDIT 2.5: hard wait used to be 500 ms — exactly the SlowSensorTask
    // poll interval, so a task could wake post-shutdown into freed memory.
    // Poll eTaskGetState() == eDeleted on each task with a budget that
    // covers worst-case poll (1.5 × 500 ms slow-sensor) plus blocking sensor
    // reads (SDS011 / PMS5003 ~2 s). 4 s ceiling.
    constexpr uint32_t WAIT_MS  = 4000;
    constexpr uint32_t STEP_MS  = 50;
    uint32_t waitDeadline = millis() + WAIT_MS;
    TaskHandle_t* handles[] = { &hSensor, &hSlowSensor, &hProcess,
                                &hStorage, &hExport };
    for (TaskHandle_t* hp : handles) {
        if (*hp == nullptr) continue;
        bool deleted = false;
        while (millis() < waitDeadline) {
            if (eTaskGetState(*hp) == eDeleted) { deleted = true; break; }
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        }
        // R12 Gemini MEDIUM: if the task didn't self-delete within budget
        // it's stuck in blocking I/O (e.g. SDS011 frame wait). Force-delete
        // so a re-init doesn't end up with two instances of the same task
        // racing for the same queues / mutexes.
        if (!deleted) {
            Serial.println("[TaskManager] shutdown: task timeout — forcing vTaskDelete");
            vTaskDelete(*hp);
        }
        *hp = nullptr;
    }
}

// ---------------------------------------------------------------------------
// checkHealth() — software watchdog (C4)
// Called from loop(). If any task hasn’t updated its heartbeat in 30s,
// set shouldRestart to trigger a graceful reboot.
// ---------------------------------------------------------------------------
bool TaskManager::checkHealth() {
    if (!running) return true;
    // Grace period: skip watchdog checks for the first 60s after boot.
    // SDS011 in periodic mode may wait 60-90s for its first frame.
    constexpr uint32_t GRACE_PERIOD_MS = 60000;
    constexpr uint32_t MAX_SILENCE_MS  = 30000;
    uint32_t now = millis();
    if (now < GRACE_PERIOD_MS) return true;
    TaskHandle_t taskHandles[TASK_COUNT] = {
        hSensor, hSlowSensor, hProcess, hStorage, hExport
    };
    for (int i = 0; i < TASK_COUNT; i++) {
        TaskHandle_t h = taskHandles[i];
        if (h && eTaskGetState(h) == eDeleted) {
            Serial.printf("[Watchdog] Task %d deleted unexpectedly\n", i);
            return false;
        }
        uint32_t hb = g_taskHeartbeat[i];
        if (now - hb > MAX_SILENCE_MS) {
            Serial.printf("[Watchdog] Task %d stuck (%lums)\n", i, now - hb);
            return false;
        }
    }
    return true;
}
