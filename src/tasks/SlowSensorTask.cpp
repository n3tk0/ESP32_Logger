#include "SlowSensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcValid
#include <time.h>

// ---------------------------------------------------------------------------
void slowSensorTaskFunc(void* /*param*/) {
    Serial.println("[SlowSensorTask] started");

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

        // Only dispatch blocking sensors (SDS011, PMS5003, WindSensor)
        sensorManager.tickFiltered(sensorQueue, ts, true);

        // Slow sensors have their own read intervals; poll every 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Serial.println("[SlowSensorTask] stopped");
    vTaskDelete(nullptr);
}
