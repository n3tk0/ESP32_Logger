// ============================================================================
// src/core/HeapWatch.h — what was running when the heap ran low, and where
// readings were lost
//
// /api/diag reports min_free_heap and queue_drops, but neither says when or
// why. A 34-hour diag showed min_free_heap 1496 B (one allocation away from a
// crash) and 7 lost readings, with nothing to go on.
//
// So the places that use a lot of heap say what they are doing (activity
// marks), loop() polls the allocator's own low-water mark, and the first time
// it falls under HEAP_WATCH_LOW_BYTES (and at each further 1 KB) a line goes
// to the event log with the time, the free heap, the largest block and the
// activities running or started in the last few seconds. Lost readings are
// counted with the place that lost them and logged at most once a minute.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef HEAP_WATCH_LOW_BYTES
#  define HEAP_WATCH_LOW_BYTES 8192
#endif

enum HeapActivityKind : uint8_t {
    HA_WEB,        // last web request (path)
    HA_EXPORT,     // an exporter's send (exporter name)
    HA_FORECAST,   // forecast fetch
    HA_ALERT,      // MQTT alert publish
    HA_COUNT
};

/// Marks an activity as started. `what` is copied (up to 31 characters).
void heapActivityBegin(HeapActivityKind k, const char* what);
/// Marks it finished; it is still named in a line for a few seconds after.
void heapActivityEnd(HeapActivityKind k);

/// Begin/end for a scope.
struct HeapActivity {
    HeapActivityKind k;
    HeapActivity(HeapActivityKind kind, const char* what) : k(kind) { heapActivityBegin(kind, what); }
    ~HeapActivity() { heapActivityEnd(k); }
    HeapActivity(const HeapActivity&) = delete;
    HeapActivity& operator=(const HeapActivity&) = delete;
};

/// `n` readings lost at `where` (a string literal). Adds to g_queueDrops.
/// Safe from any task, not from an ISR.
void dataLost(const char* where, uint32_t n = 1);

/// From loop(): checks the heap low-water mark and pending lost readings,
/// and writes the event-log lines. Cheap; does its work at most every 500 ms.
void heapWatchTick(uint32_t nowMs);
