#include "ProcessingTask.h"
#include "TaskManager.h"
#include "../setup.h"          // MODULE_HEATER_ENABLED (compile-time toggle)
#include "../pipeline/DataPipeline.h"
#ifdef FEATURE_KINDLE_DASHBOARD
#  include "../pipeline/TrendRing.h"
#endif
#include "../core/SensorTypes.h"
#include "../alerts/AlertEngine.h"
#include "../utils/MutexGuard.h"
#ifdef MODULE_HEATER_ENABLED
#  include "../modules/HeaterModule.h"
#endif
#include <math.h>

// ---------------------------------------------------------------------------
// Simple range validation — reject obvious hardware errors
static bool isPlausible(const SensorReading& r) {
    if (!isfinite(r.value)) return false;

    // Per-metric sanity bounds (extend as needed)
    const char* m = r.metric;
    if (strcmp(m, "temperature") == 0) return (r.value > -50.0f && r.value < 100.0f);
    if (strcmp(m, "humidity")    == 0) return (r.value >= 0.0f  && r.value <= 100.0f);
    // Same bounds as their parent metrics — derived, but from the same physics.
    if (strcmp(m, "dew_point")        == 0) return (r.value > -50.0f && r.value < 100.0f);
    if (strcmp(m, "humidity_amb")     == 0) return (r.value >= 0.0f  && r.value <= 100.0f);
    // Fault bitfield, not a measurement: any value is meaningful, including 0.
    // Bounded only against a corrupt frame (SPS30Sensor packs 4 bits).
    if (strcmp(m, "device_status")    == 0) return (r.value >= 0.0f  && r.value <= 255.0f);
    if (strcmp(m, "pressure")    == 0) return (r.value > 500.0f && r.value < 1200.0f);
    if (strcmp(m, "pm25")        == 0) return (r.value >= 0.0f  && r.value < 2000.0f);
    if (strcmp(m, "pm10")        == 0) return (r.value >= 0.0f  && r.value < 2000.0f);
    if (strcmp(m, "tvoc")        == 0) return (r.value >= 0.0f  && r.value < 65535.0f);
    if (strcmp(m, "eco2")        == 0) return (r.value >= 400.0f&& r.value < 65535.0f);
    if (strcmp(m, "aqi")         == 0) return (r.value >= 1.0f  && r.value <= 5.0f);
    if (strcmp(m, "flow_rate")   == 0) return (r.value >= 0.0f  && r.value < 1000.0f);
    if (strcmp(m, "wind_speed")  == 0) return (r.value >= 0.0f  && r.value < 150.0f);
    if (strcmp(m, "co2")         == 0) return (r.value >= 400.0f && r.value < 10000.0f);
    if (strcmp(m, "voltage")     == 0) return (r.value >= 0.0f  && r.value < 1000.0f);
    if (strcmp(m, "current")     == 0) return (r.value >= 0.0f  && r.value < 1000.0f);
    if (strcmp(m, "uv_index")    == 0) return (r.value >= 0.0f  && r.value <= 20.0f);
    if (strcmp(m, "lux")         == 0) return (r.value >= 0.0f  && r.value < 150000.0f);
    if (strcmp(m, "light")       == 0) return (r.value >= 0.0f  && r.value < 150000.0f);
    if (strcmp(m, "distance")    == 0) return (r.value >= 0.0f  && r.value < 50000.0f);
    if (strcmp(m, "soil_moisture")== 0) return (r.value >= 0.0f && r.value <= 100.0f);
    // Unknown metric — pass through
    return true;
}

// ---------------------------------------------------------------------------
void processingTaskFunc(void* /*param*/) {
    Serial.println("[ProcessingTask] started");

    // See SensorTask: park until init() opens the start gate so a higher-prio
    // task can't self-delete by observing running==false mid-init.
    if (!TaskManager::waitForStart()) { Serial.println("[ProcessingTask] stopped"); vTaskDelete(nullptr); return; }

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_PROCESS] = millis();   // C4 heartbeat

#ifdef MODULE_HEATER_ENABLED
        // Heater control loop. Deliberately placed BEFORE the queue receive so
        // it still runs on the 100 ms timeout path: its most important job is
        // the fail-safe that forces the output off when the enclosure probe
        // goes silent, and that is exactly the case where no readings arrive to
        // drive the loop. Internally rate-limited to 1 Hz.
        HeaterModule::instance().tick(millis());
#endif

        // Block up to 100ms waiting for a reading
        if (xQueueReceive(sensorQueue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Validate
        if (!isPlausible(r)) {
            r.quality = QUALITY_ERROR;
            // Still log errors to storage (with q=3) but skip export
        }

        // Write to web ring buffer — skip error readings so the dashboard
        // never mixes bad values with good ones.  (AUDIT 11.3)
        if (r.quality != QUALITY_ERROR) {
            MutexGuard g(webDataMutex, pdMS_TO_TICKS(5));
            if (g.isLocked()) {
                webRingBuf.push(r);
            } else {
                g_ringPushDrops++;
            }
        }

#ifdef FEATURE_KINDLE_DASHBOARD
        // Fold into the 24-hour hourly grid. Deliberately OUTSIDE the
        // webDataMutex block above: TrendRing has its own spinlock, and a
        // reading dropped from the web ring because the mutex was busy is
        // still a real measurement that the long-horizon trend should keep.
        if (r.quality != QUALITY_ERROR) {
            trendRing.add(r);
        }
#endif

        // Alert evaluation — skip when timestamp has no real wall-clock value
        // (ts < 1e9 means we're in millis-fallback territory; AlertEngine's
        // duration accounting would false-trip on ~1 s "elapsed" intervals).
        // Also skip QUALITY_ERROR readings.  (AUDIT 11.4)
        if (r.quality != QUALITY_ERROR && r.timestamp >= 1000000000u) {
            alertEngine.evaluate(r, r.timestamp);
        }

        // Forward to storage (always, even errors — raw data is immutable)
        if (xQueueSend(storageQueue, &r, pdMS_TO_TICKS(50)) != pdTRUE) {
            g_queueDrops++;  // storageQueue full — count drop (N8)
        }

        // Forward to export (only good data)
        if (r.quality != QUALITY_ERROR) {
            // R12 / AUDIT 2.9: was timeout 0 → silent drops on every WiFi
            // backpressure event. 10ms matches the storageQueue path above
            // and is short enough not to starve other sensors' enqueues.
            // Drops still counted via g_queueDrops for /api/diag visibility.
            if (xQueueSend(exportQueue, &r, pdMS_TO_TICKS(10)) != pdTRUE) {
                g_queueDrops++;
            }
        }
    }

#ifdef MODULE_HEATER_ENABLED
    // The task that drives the actuator leaves it safe when it stops. This is
    // the second of two guards — TaskManager::shutdown() also stops the heater
    // up front — because the loop can also end without shutdown() being the
    // cause, and because stop() is idempotent so the overlap costs nothing.
    HeaterModule::instance().stop();
#endif

    Serial.println("[ProcessingTask] stopped");
    vTaskDelete(nullptr);
}
