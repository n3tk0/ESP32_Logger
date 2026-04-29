#include "SlowSensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcAvailable
#include <time.h>

// ---------------------------------------------------------------------------
void slowSensorTaskFunc(void* /*param*/) {
    Serial.println("[SlowSensorTask] started");

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();   // C4 heartbeat

        // Get current Unix timestamp (same logic as SensorTask)
        uint32_t ts = 0;
        if (Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid()) ts = now.Unix32Time();
        }
        if (ts == 0) {
            time_t sysNow = 0; time(&sysNow);
            if (sysNow > 1000000000UL) ts = (uint32_t)sysNow;
        }
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

        // Only dispatch blocking sensors (SDS011, PMS5003, WindSensor)
        sensorManager.tickFiltered(sensorQueue, ts, true);

        // Slow sensors have their own read intervals; poll every 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Serial.println("[SlowSensorTask] stopped");
    vTaskDelete(nullptr);
}
