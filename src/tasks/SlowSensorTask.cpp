#include "SlowSensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcValid
#include "../setup.h"         // SLOW_SENSOR_TICK_MS
#include <time.h>

// ---------------------------------------------------------------------------
void slowSensorTaskFunc(void* /*param*/) {
    Serial.println("[SlowSensorTask] started");

    // See SensorTask: park until init() opens the start gate so a higher-prio
    // task can't self-delete by observing running==false mid-init.
    if (!TaskManager::waitForStart()) { Serial.println("[SlowSensorTask] stopped"); vTaskDelete(nullptr); return; }

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();   // C4 heartbeat

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
        // +1 avoids ts=0 (reserved sentinel).  (AUDIT 10.3)
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL) + 1;

        // Only dispatch blocking sensors (SDS011, PMS5003, WindSensor).
        // tickFiltered can block 1.5-3 s for UART frame waits; refresh the
        // heartbeat afterwards so the 30-s watchdog isn't false-tripped if
        // the polling cadence is ever tightened.  (AUDIT 10.5)
        sensorManager.tickFiltered(sensorQueue, ts, true);
        g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();

        // Poll cadence — configurable via SLOW_SENSOR_TICK_MS.  (AUDIT 10.4)
        vTaskDelay(pdMS_TO_TICKS(SLOW_SENSOR_TICK_MS));
    }

    Serial.println("[SlowSensorTask] stopped");
    vTaskDelete(nullptr);
}
