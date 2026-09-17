#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // (the clock now lives in pipelineNowEpoch)

// ---------------------------------------------------------------------------
void sensorTaskFunc(void* /*param*/) {
    Serial.println("[SensorTask] started");

    // Wait until init() has finished building the pipeline and opened the start
    // gate. Without this the scheduler preempts init() here (this task is higher
    // priority than loop), running is still false, and the task self-deletes.
    if (!TaskManager::waitForStart()) { Serial.println("[SensorTask] stopped"); vTaskDelete(nullptr); return; }

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SENSOR] = millis();   // C4 heartbeat

        // R28 / AUDIT 10.1: re-read poll interval each iteration so
        // /api/config/platform reloads pick up new sensor intervals without
        // a reboot. minReadIntervalMs() acquires configMutex internally
        // (added in PR #106 follow-up) so the iteration is safe against a
        // concurrent reloadConfig() rebuilding _sensors[].
        uint32_t pollMs = sensorManager.minReadIntervalMs();
        if (pollMs < 50) pollMs = 50;

        // One clock for every producer — the ordering and the reason it is
        // that way round live in TaskManager.h beside pipelineNowEpoch().
        const uint32_t ts = pipelineNowEpoch();

        sensorManager.tickFiltered(sensorQueue, ts, false);

        vTaskDelay(pdMS_TO_TICKS(pollMs));
    }

    Serial.println("[SensorTask] stopped");
    vTaskDelete(nullptr);
}
