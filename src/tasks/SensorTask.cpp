#include "SensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // Rtc, rtcAvailable

// ---------------------------------------------------------------------------
void sensorTaskFunc(void* /*param*/) {
    Serial.println("[SensorTask] started");

    while (TaskManager::running) {
        // Get current Unix timestamp
        uint32_t ts = 0;
        if (Rtc) {
            RtcDateTime now = Rtc->GetDateTime();
            if (now.IsValid()) ts = now.Unix32Time();
        }
        // Fallback: relative millis (quality = ESTIMATED, set by sensor plugin)
        if (ts == 0) ts = (uint32_t)(millis() / 1000UL);

        sensorManager.tick(sensorQueue, ts);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("[SensorTask] stopped");
    vTaskDelete(nullptr);
}
