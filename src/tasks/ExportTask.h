#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// ExportTask — feeds readings from exportQueue into ExportManager and runs
// its scheduler, which sends to each exporter on its configured interval.
// Priority: TASK_PRIO_EXPORT
// ============================================================================
void exportTaskFunc(void* param);
