#include "ExportTask.h"
#include "TaskManager.h"
#include "../setup.h"
#include "../pipeline/DataPipeline.h"
#include "../export/ExportManager.h"

// EXPORT_EXPORT_BATCH_SIZE / EXPORT_FLUSH_INTERVAL_MS are configured in setup.h.
// We accumulate readings into a local batch before dispatching to prevent
// hammering the network with single-reading requests.

// ---------------------------------------------------------------------------
void exportTaskFunc(void* /*param*/) {
    Serial.println("[ExportTask] started");

    // EXPORT_BATCH_SIZE / EXPORT_FLUSH_INTERVAL_MS are configured in setup.h.
    SensorReading batch[EXPORT_BATCH_SIZE];
    int           batchCount  = 0;
    uint32_t      lastFlushMs = millis();

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();   // C4 heartbeat

        // Short timeout so the task responds to running=false within 100ms,
        // allowing shutdown() to flush the final batch before the hard wait expires.
        bool got = xQueueReceive(exportQueue, &r,
                                  pdMS_TO_TICKS(100)) == pdTRUE;
        if (got) {
            if (batchCount >= EXPORT_BATCH_SIZE) {
                // Flush full batch before accepting new reading
                exportManager.sendAll(batch, batchCount);
                batchCount  = 0;
                lastFlushMs = millis();
            }
            batch[batchCount++] = r;
        }

        bool batchFull     = (batchCount >= EXPORT_BATCH_SIZE);
        bool timeoutElapsed= (millis() - lastFlushMs) >= EXPORT_FLUSH_INTERVAL_MS;

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
