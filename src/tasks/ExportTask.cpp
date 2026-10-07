#include "ExportTask.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include "TaskManager.h"
#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif
#include "../setup.h"
#include "../pipeline/DataPipeline.h"
#include "../export/ExportManager.h"

// Every reading off exportQueue goes into ExportManager's latest-value table;
// ExportManager::tick() sends to each exporter on its configured interval
// (platform_config.json export.defaults / export.<name>), restricted to the
// sensors selected for it. See ExportManager.h.

// ---------------------------------------------------------------------------
void exportTaskFunc(void* /*param*/) {
    Log.println("[ExportTask] started");

    // See SensorTask: park until init() opens the start gate. ExportTask shares
    // loop's priority so it wouldn't preempt today, but gating it keeps every
    // pipeline task consistent and safe if priorities are ever retuned.
    if (!TaskManager::waitForStart()) { Log.println("[ExportTask] stopped"); vTaskDelete(nullptr); return; }

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
        // Then drain whatever else is already queued without waiting, so a
        // burst (remote node backfill) doesn't trickle in at one per 100 ms.
        if (xQueueReceive(exportQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            int drained = 0;
            do {
                exportManager.ingest(r);
            } while (++drained < 32 && xQueueReceive(exportQueue, &r, 0) == pdTRUE);
        }

        // WHEN to send is configuration (export.defaults.interval_ms and the
        // per-exporter overrides), resolved by ExportManager — not this task.
        exportManager.tick();
    }

    // Skip flush-on-exit: sendAll blocks TLS HTTP inside the task-exit path and
    // ESP.restart() can race the TLS connection close.  (AUDIT 2.18)

    Log.println("[ExportTask] stopped");
    vTaskDelete(nullptr);
}
