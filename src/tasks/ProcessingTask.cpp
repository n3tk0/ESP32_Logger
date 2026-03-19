#include "ProcessingTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../core/SensorTypes.h"
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
        // Block up to 100ms waiting for a reading
        if (xQueueReceive(sensorQueue, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Validate
        if (!isPlausible(r)) {
            r.quality = QUALITY_ERROR;
            // Still log errors to storage (with q=3) but skip export
        }

        // Write to web ring buffer (non-blocking, best-effort)
        if (xSemaphoreTake(webDataMutex, 0) == pdTRUE) {
            webRingBuf.push(r);
            xSemaphoreGive(webDataMutex);
        }

        // Forward to storage (always, even errors — raw data is immutable)
        xQueueSend(storageQueue, &r, pdMS_TO_TICKS(50));

        // Forward to export (only good data)
        if (r.quality != QUALITY_ERROR) {
            if (xQueueSend(exportQueue, &r, 0) != pdTRUE) {
                g_queueDrops++;  // exportQueue full — count silent drop (#3)
            }
        }
    }

    Serial.println("[ProcessingTask] stopped");
    vTaskDelete(nullptr);
}
