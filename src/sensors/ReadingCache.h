// ============================================================================
// src/sensors/ReadingCache.h
//
// Small fixed-size "latest value per (sensorId, metric)" table.
//
// WHY
// ---
// Readings normally flow one way: plugin → sensorQueue → ProcessingTask →
// storage/export.  Nothing in that path lets one subsystem ask "what did
// another sensor read most recently?", and two consumers now need exactly
// that:
//
//   • BME688Sensor — needs a DS18B20's air temperature to convert its own
//     dew point back into ambient RH (see utils/Psychrometrics.h).  The
//     DS18B20 is blocking, so it runs on SlowSensorTask while the BME688
//     runs on SensorTask: a plain member variable would race.
//
//   • HeaterModule — needs enclosure temperature and dew point to drive the
//     control loop from ProcessingTask.
//
// SensorManager::tickFiltered() populates the table as readings are produced,
// before they are queued, so the cache is fed from whichever task produced
// the reading and stays valid even when the downstream queues back up.
//
// THREAD SAFETY
// -------------
// Writers: SensorTask and SlowSensorTask (both inside tickFiltered).
// Readers: any task — BME688Sensor reads it from *inside* tickFiltered, so
// this must not reuse configMutex, which tickFiltered already holds; a
// second take of that non-recursive mutex would deadlock.
//
// A portMUX spinlock is used instead: the critical sections are a bounded
// scan of at most MAX_ENTRIES short string compares (single-digit µs), it is
// safe across both cores, and it cannot participate in a lock-ordering cycle
// with the FreeRTOS mutexes because nothing else is acquired while held.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "../core/SensorTypes.h"

class ReadingCache {
public:
    // MAX_SENSORS (16) × a few metrics each would overflow any reasonable
    // table, so this caps at the number of DISTINCT series worth tracking.
    // Entries are claimed first-come; once full, put() only updates series
    // already present (see put()).  32 covers every realistic build.
    static constexpr int MAX_ENTRIES = 32;

    /// Record `r` as the latest value for its (sensorId, metric) pair.
    /// Overwrites an existing entry, otherwise claims a free slot.
    /// Readings with a non-finite value are ignored so a garbage sample
    /// cannot evict or poison a good cached series.
    void put(const SensorReading& r);

    // ------------------------------------------------------------------
    // get()
    //   Looks up the latest value for `sensorId`/`metric`.
    //   `outValue` — the cached value (untouched when the lookup fails).
    //   `outAgeMs` — millis() elapsed since it was recorded; callers MUST
    //                check this. A stale entry is the normal symptom of a
    //                sensor that has died, and the heater fail-safe depends
    //                on noticing it.
    //   Returns false if the series was never seen.
    // ------------------------------------------------------------------
    bool get(const char* sensorId, const char* metric,
             float& outValue, uint32_t& outAgeMs) const;

    /// Drop every entry. Called on sensor config reload so values belonging
    /// to sensors that no longer exist cannot be read back as if they were
    /// live (they would keep a stale age until the table filled).
    void clear();

private:
    struct Entry {
        char     sensorId[17] = {};
        char     metric[16]   = {};
        float    value        = 0.0f;
        uint32_t ms           = 0;      // millis() at record time
        bool     used         = false;
    };

    Entry _entries[MAX_ENTRIES];
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

// Global singleton — defined in ReadingCache.cpp
extern ReadingCache readingCache;
