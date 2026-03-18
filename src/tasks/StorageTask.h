#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// Context passed to StorageTask at creation
struct StorageTaskParam {
    fs::FS* fs;
};

// ============================================================================
// StorageTask — drains storageQueue and writes JSON lines to filesystem.
// Priority: TASK_PRIO_STORAGE
// ============================================================================
void storageTaskFunc(void* param);
