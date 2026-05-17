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
volatile bool     TaskManager::running     = false;

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
    TaskManager::running = false;
}

bool TaskManager::init(fs::FS& fs) {
    // AUDIT 2.3: do NOT set running=true here. Tasks + queues + mutexes are
    // built below; if any step fails, half-built state would have left
    // running=true with NULL queues, racing the rest of the system.
    // running is set ONLY at the bottom, just before return true.

    // Zero heartbeats — stale values survive warm reboots (RTC_SW_CPU_RST)
    // and cause immediate false-positive watchdog triggers (#C4).
    for (int i = 0; i < TASK_COUNT; i++) g_taskHeartbeat[i] = 0;

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

    // Parse storage config from platform_config.json (#8)
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
    storageParam.fs     = &fs;
    storageParam.logDir = s_logDir;

    // Wide-CSV pipeline knobs sourced from DeviceConfig (set via web UI).
    storageParam.csvLoggingEnabled         = config.logger.csvLoggingEnabled;
    storageParam.aggregationIntervalSec    = config.logger.aggregationIntervalSec
                                                ? config.logger.aggregationIntervalSec : 60;
    storageParam.humidityCorrectionEnabled = config.logger.humidityCorrectionEnabled;
    storageParam.humidityCorrectionKappa   = (config.logger.humidityCorrectionKappa > 0.0f)
                                                ? config.logger.humidityCorrectionKappa : 0.35f;

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
    return true;
}

// ---------------------------------------------------------------------------
void TaskManager::shutdown() {
    running = false;

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
    for (int i = 0; i < TASK_COUNT; i++) {
        uint32_t hb = g_taskHeartbeat[i];
        if (hb == 0) continue;   // task has not started yet
        if (now - hb > MAX_SILENCE_MS) {
            Serial.printf("[Watchdog] Task %d stuck (%lums)\n", i, now - hb);
            return false;
        }
    }
    return true;
}
