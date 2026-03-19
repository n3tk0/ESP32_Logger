#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../storage/JsonLogger.h"

// ---------------------------------------------------------------------------
void storageTaskFunc(void* param) {
    Serial.println("[StorageTask] started");

    auto* p = static_cast<StorageTaskParam*>(param);
    JsonLogger logger;

    if (p && p->fs) {
        // Use logDir/maxSizeKB/rotateDaily from config if provided (#8)
        logger.begin(*p->fs,
                     p->logDir    ? p->logDir    : "/logs",
                     p->maxSizeKB > 0 ? p->maxSizeKB : 512,
                     p->rotateDaily);
    } else {
        Serial.println("[StorageTask] No filesystem — storage disabled");
    }

    SensorReading r;
    while (TaskManager::running) {
        if (xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
            logger.write(r);
        }
    }

    logger.flush();
    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
