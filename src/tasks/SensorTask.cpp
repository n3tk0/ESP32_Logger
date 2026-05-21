#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcValid
#include <time.h>             // time() — NTP system clock fallback

// ---------------------------------------------------------------------------
void sensorTaskFunc(void* /*param*/) {
    Serial.println("[SensorTask] started");

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SENSOR] = millis();   // C4 heartbeat

        // R28 / AUDIT 10.1: re-read poll interval each iteration so
        // /api/config/platform reloads pick up new sensor intervals without
        // a reboot. minReadIntervalMs() acquires configMutex internally
        // (added in PR #106 follow-up) so the iteration is safe against a
        // concurrent reloadConfig() rebuilding _sensors[].
        uint32_t pollMs = sensorManager.minReadIntervalMs();
        if (pollMs < 50) pollMs = 50;

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
        // +1 avoids ts=0 (reserved as "unknown" by SensorTypes.h and
        // LiveAggregator._lastFlushEpoch sentinel).  (AUDIT 10.3)
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL) + 1;

        sensorManager.tickFiltered(sensorQueue, ts, false);

        vTaskDelay(pdMS_TO_TICKS(pollMs));
    }

    Serial.println("[SensorTask] stopped");
    vTaskDelete(nullptr);
}
