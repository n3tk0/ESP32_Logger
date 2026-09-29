#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/SensorTypes.h"
#include "../storage/DatalogFormat.h"

// ============================================================================
// FlowRunLogger — per-fill flowmeter logger for PLATFORM_HYBRID.
//
// Subscribes to sensor readings as they pass through StorageTask and detects
// "runs" — periods of non-zero flow bracketed by idle gaps.  Each completed
// run becomes one data log row (trigger FLOW: start, end, volume), which
// StorageTask collects with takeRun() and writes with its sensor rows — see
// src/storage/Datalog.h.
//
// State machine
//   IDLE    : flow_rate < startThreshold for the moment
//   RUNNING : flow_rate >= startThreshold; updates running stats on every feed
//
// Run end is detected by tick(): if the most recent non-zero flow_rate is
// older than idleTimeoutSec, the run is closed and a row written.  Volume is
// computed from the cumulative `volume` metric delta (start vs. last seen).
//
// Used by PLATFORM_HYBRID only.  PLATFORM_LEGACY uses the existing
// DataLogger.cpp run logger.  PLATFORM_CONTINUOUS streams flow readings
// as data log sensor columns like any other sensor.
//
// Thread safety: feed() and tick() share an internal mutex; the StorageTask
// is the only writer in practice, but the mutex makes the class safe to
// query from the web UI in a future chunk.
// ============================================================================
class FlowRunLogger {
public:
    FlowRunLogger();
    ~FlowRunLogger();

    FlowRunLogger(const FlowRunLogger&)            = delete;
    FlowRunLogger& operator=(const FlowRunLogger&) = delete;

    void setIdleTimeoutSec(uint32_t s) { _idleTimeoutSec = s ? s : 5; }
    void setStartThreshold(float lmin) { _startThreshold = (lmin > 0.0f) ? lmin : 0.5f; }

    // Called for every SensorReading.  Only flow_rate / volume readings are
    // observed; everything else is ignored.  `epoch` is the current Unix
    // timestamp from StorageTask (RTC -> NTP -> millis fallback).
    void feed(const SensorReading& r, uint32_t epoch);

    // Called periodically by StorageTask.  Closes the in-progress run when
    // flow has been below the threshold for idleTimeoutSec.
    void tick(uint32_t epoch);

    // The last completed run, once: true and `out` filled when a run has
    // closed since the previous call.
    bool takeRun(DatalogRow& out);

private:
    enum State : uint8_t { IDLE = 0, RUNNING = 1 };

    uint32_t _idleTimeoutSec  = 5;
    float    _startThreshold  = 0.5f;   // L/min

    State    _state           = IDLE;
    uint32_t _runStart        = 0;
    uint32_t _lastNonZeroTs   = 0;
    float    _maxFlow         = 0.0f;
    double   _flowSum         = 0.0;
    uint32_t _flowCount       = 0;
    float    _volumeStart     = NAN;
    float    _volumeLatest    = NAN;
    DatalogRow _done          = {};
    bool     _hasDone         = false;

    mutable SemaphoreHandle_t _mutex = nullptr;

    void _closeRun(uint32_t endTs);   // caller holds _mutex
};
