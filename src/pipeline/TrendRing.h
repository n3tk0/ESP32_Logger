// ============================================================================
// src/pipeline/TrendRing.h
//
// 24 hourly buckets per tracked series, for the long-horizon trend on the
// Kindle dashboard.
//
// WHY NOT JUST READ webRingBuf
// ----------------------------
// Because it does not go back far enough, and on the smallest target it is
// not close. webRingBuf holds a fixed byte budget of raw readings — about
// 227 entries on a C3. A build emitting ~19 metrics every 10 s fills that in
// roughly TWO MINUTES. Even the 4 MB PSRAM ring on an S3 reaches about eight
// hours, and the FS-backed history that would cover the rest is still stubbed
// out ("FS query disabled until the wide-CSV reader ships").
//
// So a 24-hour trend cannot be a query over existing storage. It needs its
// own, and the cheapest correct shape is a fixed grid of hourly aggregates:
//
//   4 series × 24 hours × 12 bytes ≈ 1.2 KB of RAM, constant.
//
// That is affordable on every target in the matrix, including the C3, and it
// is what makes the dashboard's headline feature work on the board the user
// actually has rather than only on the one with PSRAM.
//
// WHAT IT KEEPS
// -------------
// min / max / mean per hour, not raw samples. A 6" e-ink panel at the size
// this renders cannot resolve more than a couple of hundred horizontal
// pixels of line anyway, and min/max is what makes an overnight frost
// visible — a mean-only trend hides exactly the excursion you want to see.
//
// TIME BASE
// ---------
// Buckets are indexed by absolute Unix hour (ts / 3600) modulo 24, so the
// ring self-advances with wall-clock time and needs no timer. A reading
// whose hour is older than the 24-hour window is dropped rather than folded
// into whatever bucket it collides with; a reading from a NEW hour clears
// that slot before accumulating. Readings arriving before NTP has set the
// clock (ts < 1e9) are rejected outright — they would land in a bucket
// derived from a 1970 timestamp and corrupt the grid.
//
// THREAD SAFETY
// -------------
// Writer: ProcessingTask, one reading at a time.
// Readers: the AsyncTCP task rendering /kindle.
//
// A portMUX spinlock, consistent with ReadingCache and RemoteIngest: the
// critical sections are a bounded scan over at most MAX_SERIES short string
// compares plus arithmetic, and nothing else is acquired while it is held.
// Notably it is NOT webDataMutex — ProcessingTask already contends for that
// on every push with a 5 ms timeout, and adding a second consumer to it
// would turn dashboard renders into dropped readings.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "../core/SensorTypes.h"

class TrendRing {
public:
    static constexpr int HOURS      = 24;
    static constexpr int MAX_SERIES = 4;

    struct Hour {
        float    min;
        float    max;
        float    sum;
        uint16_t count;   // 0 = no data for this hour
    };

    /// Start tracking (sensorId, metric). Returns false when all MAX_SERIES
    /// slots are taken. Registering the same pair twice is a no-op success.
    /// Call from setup, before ProcessingTask starts feeding the ring.
    bool track(const char* sensorId, const char* metric);

    /// Fold one reading into its hour bucket. Ignores readings for untracked
    /// series, non-finite values, and timestamps before 2001 (no NTP yet).
    void add(const SensorReading& r);

    /// Copy the 24 buckets for a tracked series into `out`, oldest first,
    /// ending with the hour containing `nowTs`. Buckets with count == 0 are
    /// gaps — the caller decides whether to interpolate or break the line.
    /// Returns false when the series is not tracked.
    bool series(const char* sensorId, const char* metric, uint32_t nowTs,
                Hour* out) const;

private:
    struct Series {
        char     sensorId[17];
        char     metric[16];
        bool     used;
        uint32_t lastHour;          // absolute Unix hour last written
        Hour     h[HOURS];
    };

    int _find(const char* sensorId, const char* metric) const;

    Series _s[MAX_SERIES] = {};
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern TrendRing trendRing;
