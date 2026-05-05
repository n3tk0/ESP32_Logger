#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// ============================================================================
// StorageTask — drains storageQueue and persists sensor readings.
//
// Two pipelines, selected at runtime via `csvLoggingEnabled`:
//
//   ┌───────────────┬─────────────────────────────────────────────────────────┐
//   │ legacy (0)    │ JsonLogger writes one JSONL line per reading            │
//   │ wide-CSV (1)  │ LiveAggregator buffers in RAM; CsvLogger emits a single │
//   │               │ wide row every aggregationIntervalSec                   │
//   └───────────────┴─────────────────────────────────────────────────────────┘
//
// Both pipelines respect fsMutex for serialised filesystem access and never
// allocate during the steady-state write path (snprintf + static buffers).
// ============================================================================
struct StorageTaskParam {
    fs::FS*     fs;
    const char* logDir      = "/logs";   // log directory path
    uint32_t    maxSizeKB   = 512;       // per-file size cap before rotation
    bool        rotateDaily = true;      // daily file rotation (vs. size only)
    fs::FS*     mirrorFS    = nullptr;   // optional secondary FS for dual-write

    // Wide-CSV pipeline knobs (used only when csvLoggingEnabled == true)
    bool        csvLoggingEnabled         = false;
    uint16_t    aggregationIntervalSec    = 60;
    bool        humidityCorrectionEnabled = false;
    float       humidityCorrectionKappa   = 0.35f;
};

void storageTaskFunc(void* param);
