#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// Context passed to StorageTask at creation
struct StorageTaskParam {
    fs::FS*     fs;
    const char* logDir      = "/logs";  // log directory path (#8)
    uint32_t    maxSizeKB   = 512;      // per-file size limit before rotation
    bool        rotateDaily = true;     // daily rotation (vs. size-only)
};

// ============================================================================
// StorageTask — drains storageQueue and writes JSON lines to filesystem.
// Priority: TASK_PRIO_STORAGE
// ============================================================================
void storageTaskFunc(void* param);
