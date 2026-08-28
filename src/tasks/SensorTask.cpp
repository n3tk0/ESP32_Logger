#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcValid
#include <time.h>             // time() — NTP system clock fallback

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

        // Timestamp priority: system clock → hardware RTC → millis monotonic.
        //
        // THE SYSTEM CLOCK LEADS, AND THE ORDER USED TO BE THE OTHER WAY.
        //
        // Preferring the RTC looked obviously right — it is the clock that
        // survives a power cut — but it made every reading carry a stamp from
        // one clock while ProcessingTask judged it against another. Nothing
        // disciplines the DS1302 to NTP and nothing seeds the system clock
        // from the DS1302, so the two drift apart with no upper bound.
        //
        // Once the RTC is more than two minutes slow, readingIsBackfilled()
        // starts calling every fresh reading history: out of the live web
        // ring, out of alert evaluation, and silent about it. A dashboard that
        // simply stops updating, weeks after anyone touched the device.
        //
        // Judged and stamped by the same clock, that cannot happen. The RTC
        // keeps the job it is actually needed for — being the only real clock
        // on a device with no network, where time(nullptr) never becomes
        // plausible and this falls through to it exactly as before.
        uint32_t ts = 0;
        time_t sysT = time(nullptr);
        if (sysT > 1000000000L) ts = (uint32_t)sysT;
        if (ts == 0 && Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid() && now.Year() >= 2020) ts = now.Unix32Time();
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
