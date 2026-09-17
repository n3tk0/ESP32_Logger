#include "SlowSensorTask.h"
#include "TaskManager.h"
#include "../sensors/SensorManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/Globals.h"  // (the clock now lives in pipelineNowEpoch)
#include "../setup.h"         // SLOW_SENSOR_TICK_MS

// ---------------------------------------------------------------------------
void slowSensorTaskFunc(void* /*param*/) {
    Serial.println("[SlowSensorTask] started");

    // See SensorTask: park until init() opens the start gate so a higher-prio
    // task can't self-delete by observing running==false mid-init.
    if (!TaskManager::waitForStart()) { Serial.println("[SlowSensorTask] stopped"); vTaskDelete(nullptr); return; }

    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();   // C4 heartbeat

        // THE SAME CLOCK SensorTask AND ProcessingTask USE. This task asked
        // the DS1302 first, which is the ordering SensorTask's comment was
        // written to explain the removal of: a drifting RTC made every
        // reading from the blocking sensors — the dust sensors and the
        // anemometer, all of them here — read as backfill, dropping them from
        // the live dashboard and from alerts without a word. See
        // pipelineNowEpoch() in TaskManager.h.
        const uint32_t ts = pipelineNowEpoch();

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
