#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// SlowSensorTask — runs blocking sensors (SDS011 ~1.5s, PMS5003 ~2s, Wind ~3s)
// at a lower priority so they cannot starve the ProcessingTask or ExportTask.
// Priority: TASK_PRIO_PROCESS (same as ProcessingTask, below SensorTask)
// ============================================================================
void slowSensorTaskFunc(void* param);
