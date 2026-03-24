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
bool TaskManager::init(fs::FS& fs) {
    running = true;

    // Dynamic queue depth: scale sensor queue to actual sensor count × 4 metrics
    // so one full tick cycle never drops readings (#3.5)
    int sCount    = sensorManager.count();
    int dynSDepth = (sCount > 0) ? max((int)QUEUE_SENSOR_DEPTH, sCount * 4)
                                 : (int)QUEUE_SENSOR_DEPTH;

    // Create queues
    sensorQueue  = xQueueCreate((UBaseType_t)dynSDepth, sizeof(SensorReading));
    storageQueue = xQueueCreate(QUEUE_STORAGE_DEPTH,    sizeof(SensorReading));
    exportQueue  = xQueueCreate(QUEUE_EXPORT_DEPTH,     sizeof(SensorReading));
    Serial.printf("[TaskManager] sensorQueue depth=%d (sensors=%d)\n",
                  dynSDepth, sCount);

    if (!sensorQueue || !storageQueue || !exportQueue) {
        Serial.println("[TaskManager] Queue creation FAILED");
        return false;
    }

    // Create mutexes
    webDataMutex = xSemaphoreCreateMutex();
    configMutex  = xSemaphoreCreateMutex();
    wireMutex    = xSemaphoreCreateMutex();   // I2C bus serialisation (#14)
    fsMutex      = xSemaphoreCreateMutex();   // FS write serialisation (FS1)

    if (!webDataMutex || !configMutex || !wireMutex || !fsMutex) {
        Serial.println("[TaskManager] Mutex creation FAILED");
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

    // Create tasks — all on core 0 (ESP32-C3 is unicore; dual-core ESP32
    // can assign WebTask to core 1 by changing tskNO_AFFINITY to 1)
    BaseType_t r;

    r = xTaskCreatePinnedToCore(sensorTaskFunc,     "SensorTask",
                                STACK_SENSOR_TASK,  nullptr,
                                TASK_PRIO_SENSOR,   &hSensor,   0);
    if (r != pdPASS) { Serial.println("[TaskManager] SensorTask FAILED"); return false; }

    r = xTaskCreatePinnedToCore(slowSensorTaskFunc,     "SlowSensorTask",
                                STACK_SLOW_SENSOR_TASK, nullptr,
                                TASK_PRIO_SLOW_SENSOR,  &hSlowSensor, 0);
    if (r != pdPASS) { Serial.println("[TaskManager] SlowSensorTask FAILED"); return false; }

    r = xTaskCreatePinnedToCore(processingTaskFunc, "ProcessTask",
                                STACK_PROCESS_TASK, nullptr,
                                TASK_PRIO_PROCESS,  &hProcess,  0);
    if (r != pdPASS) { Serial.println("[TaskManager] ProcessTask FAILED"); return false; }

    r = xTaskCreatePinnedToCore(storageTaskFunc,    "StorageTask",
                                STACK_STORAGE_TASK, &storageParam,
                                TASK_PRIO_STORAGE,  &hStorage,  0);
    if (r != pdPASS) { Serial.println("[TaskManager] StorageTask FAILED"); return false; }

    r = xTaskCreatePinnedToCore(exportTaskFunc,     "ExportTask",
                                STACK_EXPORT_TASK,  nullptr,
                                TASK_PRIO_EXPORT,   &hExport,   0);
    if (r != pdPASS) { Serial.println("[TaskManager] ExportTask FAILED"); return false; }

    Serial.println("[TaskManager] All tasks started");
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

    // Hard wait for task stacks to unwind
    vTaskDelay(pdMS_TO_TICKS(500));
}

// ---------------------------------------------------------------------------
// checkHealth() — software watchdog (C4)
// Called from loop(). If any task hasn’t updated its heartbeat in 30s,
// set shouldRestart to trigger a graceful reboot.
// ---------------------------------------------------------------------------
bool TaskManager::checkHealth() {
    if (!running) return true;
    constexpr uint32_t MAX_SILENCE_MS = 30000;
    uint32_t now = millis();
    for (int i = 0; i < TASK_COUNT; i++) {
        uint32_t hb = g_taskHeartbeat[i];
        if (hb == 0) continue;   // task hasn’t started yet
        if (now - hb > MAX_SILENCE_MS) {
            Serial.printf("[Watchdog] Task %d stuck (%lums)\n", i, now - hb);
            return false;
        }
    }
    return true;
}
