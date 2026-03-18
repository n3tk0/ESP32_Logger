#pragma once
#include <Arduino.h>
#include "../core/SensorTypes.h"

// ============================================================================
// AggregationEngine
//
// Provides three operations:
//   1. lttb()      — visual-fidelity downsampling (Largest Triangle Three Buckets)
//   2. bucket()    — time-window aggregation (avg/min/max/sum per bucket)
//   3. aggregate() — bucket → optional LTTB, combined pipeline
//
// IMPORTANT: These functions operate on copies; they never modify raw data.
// ============================================================================
class AggregationEngine {
public:
    // ------------------------------------------------------------------
    // LTTB — Largest Triangle Three Buckets
    //
    // Reduces `inLen` points to at most `maxPoints` while preserving the
    // visual shape of the time-series (Steinarsson 2013).
    //
    // Returns actual output count (always <= maxPoints).
    // If inLen <= maxPoints, copies input verbatim (no reduction).
    // ------------------------------------------------------------------
    static size_t lttb(const SensorReading* in,  size_t inLen,
                             SensorReading* out, size_t maxPoints);

    // ------------------------------------------------------------------
    // bucket()
    //
    // Groups readings into time windows of `bucketMins` minutes and
    // reduces each window to a single reading using `mode`.
    //
    // Returns number of output readings (one per non-empty bucket).
    // outMaxLen must be >= expected bucket count.
    //
    // For AGG_RAW: copies input unchanged (ignores bucket width).
    // ------------------------------------------------------------------
    static size_t bucket(const SensorReading* in,  size_t inLen,
                               SensorReading* out, size_t outMaxLen,
                               TimeBucket bucketMins, AggMode mode);

    // ------------------------------------------------------------------
    // aggregate()
    //
    // Combined pipeline:
    //   1. bucket() into time windows
    //   2. If result > maxPoints: apply LTTB
    //
    // Returns output count.
    // ------------------------------------------------------------------
    static size_t aggregate(const SensorReading* in,  size_t inLen,
                                  SensorReading* out, size_t outMaxLen,
                                  TimeBucket bucketMins,
                                  AggMode    mode,
                                  size_t     maxPoints = 500);

private:
    // Triangle area used by LTTB
    static double _triangleArea(const SensorReading& a,
                                 const SensorReading& b,
                                 const SensorReading& c);

    // Reduce a window of readings to one using AggMode
    static SensorReading _reduceWindow(const SensorReading* w,
                                        size_t wLen,
                                        AggMode mode);
};
