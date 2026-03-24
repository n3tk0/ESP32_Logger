#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../storage/JsonLogger.h"

// ---------------------------------------------------------------------------
void storageTaskFunc(void* param) {
    Serial.println("[StorageTask] started");

    auto* p = static_cast<StorageTaskParam*>(param);
    JsonLogger logger;
    JsonLogger mirrorLogger;
    bool       mirrorActive = false;

    if (p && p->fs) {
        // Use logDir/maxSizeKB/rotateDaily from config if provided (#8)
        logger.begin(*p->fs,
                     p->logDir    ? p->logDir    : "/logs",
                     p->maxSizeKB > 0 ? p->maxSizeKB : 512,
                     p->rotateDaily);
        // Mirror write to second FS if configured (#5/2.1)
        if (p->mirrorFS) {
            mirrorLogger.begin(*p->mirrorFS,
                               p->logDir    ? p->logDir    : "/logs",
                               p->maxSizeKB > 0 ? p->maxSizeKB : 512,
                               p->rotateDaily);
            mirrorActive = true;
            Serial.println("[StorageTask] Mirror write active");
        }
    } else {
        Serial.println("[StorageTask] No filesystem — storage disabled");
    }

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();   // C4 heartbeat

        if (xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);   // FS1
            logger.write(r);
            if (mirrorActive) mirrorLogger.write(r);
            if (fsMutex) xSemaphoreGive(fsMutex);
        }
    }

    if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);   // FS1
    logger.flush();
    if (mirrorActive) mirrorLogger.flush();
    if (fsMutex) xSemaphoreGive(fsMutex);
    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
