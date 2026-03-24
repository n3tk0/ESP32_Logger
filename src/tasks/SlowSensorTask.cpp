#include "SlowSensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcAvailable

// ---------------------------------------------------------------------------
void slowSensorTaskFunc(void* /*param*/) {
    DBGLN("[SlowSensorTask] started");

    while (TaskManager::running) {
        // Get current Unix timestamp (same logic as SensorTask)
        uint32_t ts = 0;
        if (Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid()) ts = now.Unix32Time();
        }
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

        // Only dispatch blocking sensors (SDS011, PMS5003, WindSensor)
        sensorManager.tickFiltered(sensorQueue, ts, true);

        // Slow sensors have their own read intervals; poll every 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    DBGLN("[SlowSensorTask] stopped");
    vTaskDelete(nullptr);
}
