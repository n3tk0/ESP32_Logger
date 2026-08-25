#include "ExportTask.h"
#include "TaskManager.h"
#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif
#include "../setup.h"
#include "../pipeline/DataPipeline.h"
#include "../export/ExportManager.h"

// EXPORT_EXPORT_BATCH_SIZE / EXPORT_FLUSH_INTERVAL_MS are configured in setup.h.
// We accumulate readings into a local batch before dispatching to prevent
// hammering the network with single-reading requests.

// ---------------------------------------------------------------------------
void exportTaskFunc(void* /*param*/) {
    Serial.println("[ExportTask] started");

    // See SensorTask: park until init() opens the start gate. ExportTask shares
    // loop's priority so it wouldn't preempt today, but gating it keeps every
    // pipeline task consistent and safe if priorities are ever retuned.
    if (!TaskManager::waitForStart()) { Serial.println("[ExportTask] stopped"); vTaskDelete(nullptr); return; }

    // EXPORT_BATCH_SIZE / EXPORT_FLUSH_INTERVAL_MS are configured in setup.h.
    SensorReading batch[EXPORT_BATCH_SIZE];
    size_t        batchCount  = 0;   // CM-4: match sendAll(…, size_t count)
    uint32_t      lastFlushMs = millis();

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();   // C4 heartbeat

#ifdef MODULE_FORECAST_ENABLED
        // The forecast fetch lives here, not on ProcessingTask, for two
        // reasons that both end in a crash otherwise:
        //
        //   Stack. It runs WiFiClientSecure + HTTPClient + getString() + a
        //   JSON parse. STACK_EXPORT_TASK is 8192 and is commented "WiFi +
        //   TLS + JSON serialisation" precisely for this shape of work;
        //   STACK_PROCESS_TASK is 6144 and would likely abort on the canary.
        //
        //   Watchdog. It blocks. Every task stamps a heartbeat at the top of
        //   its loop and TaskManager reboots the device after MAX_SILENCE_MS
        //   (30 s) of silence, so a blocking call has to finish well inside
        //   that — see the timeout and redirect limits in ForecastModule.
        //
        // ProcessingTask additionally drives the heater fail-safe and drains
        // sensorQueue (depth 20); stalling it there loses readings too.
        forecastModule.tick(millis());
#endif

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
