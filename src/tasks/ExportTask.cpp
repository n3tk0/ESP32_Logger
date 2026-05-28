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
    size_t        batchCount  = 0;   // CM-4: match sendAll(…, size_t count)
    uint32_t      lastFlushMs = millis();

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();   // C4 heartbeat

        // Short timeout so the task responds to running=false within 100ms.
        bool got = xQueueReceive(exportQueue, &r,
                                  pdMS_TO_TICKS(100)) == pdTRUE;
        // Guard against overflow (batchCount should never reach EXPORT_BATCH_SIZE
        // here, but be defensive).  (AUDIT 11.9: single decision-point)
        if (got && batchCount < EXPORT_BATCH_SIZE) batch[batchCount++] = r;

        if (batchCount > 0 &&
            (batchCount >= EXPORT_BATCH_SIZE ||
             millis() - lastFlushMs >= EXPORT_FLUSH_INTERVAL_MS)) {
            exportManager.sendAll(batch, batchCount);
            batchCount  = 0;
            lastFlushMs = millis();
        }
    }

    // Skip flush-on-exit: sendAll blocks TLS HTTP inside the task-exit path and
    // ESP.restart() can race the TLS connection close.  (AUDIT 2.18)

    Serial.println("[ExportTask] stopped");
    vTaskDelete(nullptr);
}
