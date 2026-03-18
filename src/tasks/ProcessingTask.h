#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// ProcessingTask — dequeues raw readings, validates, routes to storage + export.
//
// For each SensorReading from sensorQueue:
//   1. Normalise (unit sanity, NaN guard)
//   2. Push to webRingBuf (lock-free, best-effort)
//   3. Forward to storageQueue
//   4. Forward to exportQueue (if export is enabled)
//
// Priority: TASK_PRIO_PROCESS
// ============================================================================
void processingTaskFunc(void* param);
