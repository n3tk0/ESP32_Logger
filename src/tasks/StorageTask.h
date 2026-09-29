#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// StorageTaskParam holds only fs::FS* pointers — a forward declaration is
// enough here; <FS.h> belongs in the .cpp (header-bloat audit).
namespace fs { class FS; }

// ============================================================================
// StorageTask — drains storageQueue into the data log.
//
// Pipeline:
//   ProcessingTask → storageQueue → LiveAggregator (RAM averages per column)
//                                 → one data log row per interval (TIMER)
//   FlowRunLogger (hybrid)        → one data log row per fill (FLOW)
//
// Rows are collected DL_BATCH_ROWS at a time and written by datalogAppend()
// (src/storage/Datalog.h) under fsMutex; a flush request (restart), a
// changed header or the task's exit writes the batch early. The flowmeter
// runs of PLATFORM_LEGACY go to the same file from DataLogger.cpp.
//
// `csvLoggingEnabled = false` turns the sensor rows off — the task still
// drains the queue (so the ring buffer and exporters stay fed).
// ============================================================================
struct StorageTaskParam {
    fs::FS*     fs;
    fs::FS*     mirrorFS    = nullptr;   // optional secondary FS for dual-write

    bool        csvLoggingEnabled         = true;
    uint16_t    aggregationIntervalSec    = 60;
    bool        humidityCorrectionEnabled = false;
    float       humidityCorrectionKappa   = 0.35f;

    // FlowRunLogger — per-fill flowmeter logger for PLATFORM_HYBRID.
    // Compile-time gated by SENSOR_WATERFLOW_ENABLED in TaskManager so the
    // class can be DCE'd when no flowmeter is built in.
    bool        enableFlowRunLogger       = false;
    uint16_t    flowRunIdleTimeoutSec     = 5;
    float       flowRunStartThreshold     = 0.5f;  // L/min
};

void storageTaskFunc(void* param);
