#include "ProcessingTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/SensorTypes.h"
#include "../alerts/AlertEngine.h"
#include "../utils/MutexGuard.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Simple range validation — reject obvious hardware errors
static bool isPlausible(const SensorReading& r) {
    if (!isfinite(r.value)) return false;

    // Per-metric sanity bounds (extend as needed)
    const char* m = r.metric;
    if (strcmp(m, "temperature") == 0) return (r.value > -50.0f && r.value < 100.0f);
    if (strcmp(m, "humidity")    == 0) return (r.value >= 0.0f  && r.value <= 100.0f);
    if (strcmp(m, "pressure")    == 0) return (r.value > 500.0f && r.value < 1200.0f);
    if (strcmp(m, "pm25")        == 0) return (r.value >= 0.0f  && r.value < 2000.0f);
    if (strcmp(m, "pm10")        == 0) return (r.value >= 0.0f  && r.value < 2000.0f);
    if (strcmp(m, "tvoc")        == 0) return (r.value >= 0.0f  && r.value < 65535.0f);
    if (strcmp(m, "eco2")        == 0) return (r.value >= 400.0f&& r.value < 65535.0f);
    if (strcmp(m, "aqi")         == 0) return (r.value >= 1.0f  && r.value <= 5.0f);
    if (strcmp(m, "flow_rate")   == 0) return (r.value >= 0.0f  && r.value < 1000.0f);
    if (strcmp(m, "wind_speed")  == 0) return (r.value >= 0.0f  && r.value < 150.0f);
    // Unknown metric — pass through
    return true;
}

// ---------------------------------------------------------------------------
void processingTaskFunc(void* /*param*/) {
    Serial.println("[ProcessingTask] started");

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_PROCESS] = millis();   // C4 heartbeat

        // Block up to 100ms waiting for a reading
        if (xQueueReceive(sensorQueue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Validate
        if (!isPlausible(r)) {
            r.quality = QUALITY_ERROR;
            // Still log errors to storage (with q=3) but skip export
        }

        // Write to web ring buffer (short timeout; drop on contention)
        {
            MutexGuard g(webDataMutex, pdMS_TO_TICKS(5));
            if (g.isLocked()) {
                webRingBuf.push(r);
            } else {
                g_ringPushDrops++;
            }
        }

        // Alert evaluation — only for plausible readings (QUALITY_ERROR is
        // already the guard above; evaluate() will still run for warn-quality
        // readings so borderline values can trigger user-defined thresholds).
        if (r.quality != QUALITY_ERROR) {
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

    Serial.println("[ProcessingTask] stopped");
    vTaskDelete(nullptr);
}
