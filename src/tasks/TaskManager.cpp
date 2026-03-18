#include "TaskManager.h"
#include "SensorTask.h"
#include "ProcessingTask.h"
#include "StorageTask.h"
#include "ExportTask.h"
#include "../pipeline/DataPipeline.h"

// Static member definitions
TaskHandle_t      TaskManager::hSensor  = nullptr;
TaskHandle_t      TaskManager::hProcess = nullptr;
TaskHandle_t      TaskManager::hStorage = nullptr;
TaskHandle_t      TaskManager::hExport  = nullptr;
volatile bool     TaskManager::running  = false;

// Storage task needs a persistent param (lives for task lifetime)
static StorageTaskParam storageParam;

// ---------------------------------------------------------------------------
bool TaskManager::init(fs::FS& fs) {
    running = true;

    // Create queues
    sensorQueue  = xQueueCreate(QUEUE_SENSOR_DEPTH,  sizeof(SensorReading));
    storageQueue = xQueueCreate(QUEUE_STORAGE_DEPTH, sizeof(SensorReading));
    exportQueue  = xQueueCreate(QUEUE_EXPORT_DEPTH,  sizeof(SensorReading));

    if (!sensorQueue || !storageQueue || !exportQueue) {
        Serial.println("[TaskManager] Queue creation FAILED");
        return false;
    }

    // Create mutexes
    webDataMutex = xSemaphoreCreateMutex();
    configMutex  = xSemaphoreCreateMutex();

    if (!webDataMutex || !configMutex) {
        Serial.println("[TaskManager] Mutex creation FAILED");
        return false;
    }

    storageParam.fs = &fs;

    // Create tasks — all on core 0 (ESP32-C3 is unicore; dual-core ESP32
    // can assign WebTask to core 1 by changing tskNO_AFFINITY to 1)
    BaseType_t r;

    r = xTaskCreatePinnedToCore(sensorTaskFunc,     "SensorTask",
                                STACK_SENSOR_TASK,  nullptr,
                                TASK_PRIO_SENSOR,   &hSensor,   0);
    if (r != pdPASS) { Serial.println("[TaskManager] SensorTask FAILED"); return false; }

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
    // Give tasks time to drain and exit (2s)
    vTaskDelay(pdMS_TO_TICKS(2000));
}
