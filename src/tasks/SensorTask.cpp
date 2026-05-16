#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcValid
#include <time.h>             // time() — NTP system clock fallback

// ---------------------------------------------------------------------------
void sensorTaskFunc(void* /*param*/) {
    Serial.println("[SensorTask] started");

    // C1: compute poll interval from sensor config (min 50ms, default 1s)
    uint32_t pollMs = sensorManager.minReadIntervalMs();
    if (pollMs < 50) pollMs = 50;

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SENSOR] = millis();   // C4 heartbeat

        // Timestamp priority: hardware RTC → NTP system clock → millis monotonic
        uint32_t ts = 0;
        if (Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid() && now.Year() >= 2020) ts = now.Unix32Time();
        }
        if (ts == 0) {
            time_t sysT = time(nullptr);
            if (sysT > 1000000000L) ts = (uint32_t)sysT;
        }
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

        sensorManager.tickFiltered(sensorQueue, ts, false);

        vTaskDelay(pdMS_TO_TICKS(pollMs));
    }

    Serial.println("[SensorTask] stopped");
    vTaskDelete(nullptr);
}
