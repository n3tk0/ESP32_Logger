#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// ============================================================================
// StorageTask — drains storageQueue and persists sensor readings as wide CSV.
//
// Pipeline:
//   ProcessingTask → storageQueue → LiveAggregator (RAM, mutex-guarded)
//                                 → CsvLogger      (per-day file, fsMutex)
//
// One row per `aggregationIntervalSec` (default 60) is emitted with the
// running average per (sensorType, metric) since the last flush.  Optional
// SDS011 humidity correction (k-Köhler) is applied at feed-time inside the
// aggregator using the most recent BME-family humidity reading.
//
// Flowmeter "run" volumes for PLATFORM_LEGACY are written by a separate
// path (src/managers/DataLogger.cpp) and are unaffected by this task.
//
// `csvLoggingEnabled = false` is a kill switch for export-only deployments —
// the task still drains the queue (so the ring buffer and exporters stay
// fed) but writes nothing to the filesystem.
// ============================================================================
struct StorageTaskParam {
    fs::FS*     fs;
    const char* logDir      = "/logs";   // log directory path
    uint32_t    maxSizeKB   = 1024;      // per-file size cap before rotation
    bool        rotateDaily = true;      // (currently always true; kept for compat)
    fs::FS*     mirrorFS    = nullptr;   // optional secondary FS for dual-write

    bool        csvLoggingEnabled         = true;
    uint16_t    aggregationIntervalSec    = 60;
    bool        humidityCorrectionEnabled = false;
    float       humidityCorrectionKappa   = 0.35f;
};

void storageTaskFunc(void* param);
