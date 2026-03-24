#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcAvailable

// ---------------------------------------------------------------------------
void sensorTaskFunc(void* /*param*/) {
    Serial.println("[SensorTask] started");

    // C1: compute poll interval from sensor config (min 50ms, default 1s)
    uint32_t pollMs = sensorManager.minReadIntervalMs();

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SENSOR] = millis();   // C4 heartbeat

        // Get current Unix timestamp
        uint32_t ts = 0;
        if (Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid()) ts = now.Unix32Time();
        }
        // Fallback: relative millis (quality = ESTIMATED, set by sensor plugin)
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

        sensorManager.tickFiltered(sensorQueue, ts, false);

        vTaskDelay(pdMS_TO_TICKS(pollMs));
    }

    Serial.println("[SensorTask] stopped");
    vTaskDelete(nullptr);
}
