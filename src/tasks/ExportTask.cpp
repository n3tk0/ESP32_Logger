#include "ExportTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../export/ExportManager.h"

// Accumulate readings into a local batch before dispatching.
// Prevents hammering the network with single-reading requests.
static constexpr int  BATCH_SIZE      = 20;
static constexpr int  FLUSH_INTERVAL_MS = 60000; // 1 min max wait

// ---------------------------------------------------------------------------
void exportTaskFunc(void* /*param*/) {
    Serial.println("[ExportTask] started");

    SensorReading batch[BATCH_SIZE];
    int           batchCount  = 0;
    uint32_t      lastFlushMs = millis();

    SensorReading r;
    while (TaskManager::running) {
        bool got = xQueueReceive(exportQueue, &r,
                                  pdMS_TO_TICKS(1000)) == pdTRUE;
        if (got && batchCount < BATCH_SIZE) {
            batch[batchCount++] = r;
        }

        bool batchFull     = (batchCount >= BATCH_SIZE);
        bool timeoutElapsed= (millis() - lastFlushMs) >= FLUSH_INTERVAL_MS;

        if ((batchFull || timeoutElapsed) && batchCount > 0) {
            exportManager.sendAll(batch, batchCount);
            batchCount   = 0;
            lastFlushMs  = millis();
        }
    }

    // Flush remaining
    if (batchCount > 0) {
        exportManager.sendAll(batch, batchCount);
    }

    Serial.println("[ExportTask] stopped");
    vTaskDelete(nullptr);
}
