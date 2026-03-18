#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// ExportTask — batches readings from exportQueue and dispatches to all
// registered exporters when batch is full or flush interval elapses.
// Priority: TASK_PRIO_EXPORT
// ============================================================================
void exportTaskFunc(void* param);
