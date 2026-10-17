#pragma once
// src/core/HeapBudget.h — who may use the heap at the same time
//
// A C3 has about 60 KB of heap. A TLS export or forecast fetch wants most of
// it, a page load asks for six replies at once, the Kindle and the WiFi node
// ask on their own schedules, and the SSE channel builds a snapshot every
// second. None of them knew about the others, so the peaks added up and
// whatever allocated last failed — each library in its own way: an empty
// 200, an abort, a malloc retried per byte past the task watchdog
// (design/c3-heap-design-1016.md).
//
// So they share a budget:
//  - a dynamic web reply holds a WebUnit for as long as it exists (the
//    response object owns it, so it is released once the last byte is out);
//    none free → 503 + Retry-After, which the page asks once more after;
//  - a heavy job holds a HeavyLease: it waits for the replies in flight to
//    finish (no new ones are admitted meanwhile), up to HEAP_BUDGET_WAIT_MS,
//    and then runs anyway;
//  - the SSE push skips a second it cannot get a unit for.
//
// With PSRAM the budget is off: every request is admitted.

#include <stdint.h>

#ifndef HEAP_BUDGET_UNITS
#define HEAP_BUDGET_UNITS 3        // web replies at once
#endif
#ifndef HEAP_BUDGET_HEAVY_UNITS
#define HEAP_BUDGET_HEAVY_UNITS HEAP_BUDGET_UNITS   // a TLS job runs alone
#endif
#ifndef HEAP_BUDGET_WAIT_MS
#define HEAP_BUDGET_WAIT_MS 5000
#endif

/// From setup(), before the web server and the tasks start.
void heapBudgetBegin(bool enabled);

/// Whether a new web reply would get a unit now (the middleware's early 503,
/// before a handler builds anything).
bool heapBudgetWebAdmissible();

/// Seconds for Retry-After: longer while a heavy job runs or waits.
unsigned heapBudgetRetryAfter();

struct HeapBudgetStats {
    bool     enabled;
    int      total, used;
    bool     heavyActive, heavyWaiting;
    uint32_t webRefused;       // replies answered 503 for want of a unit
    uint32_t heavyRuns;
    uint32_t heavyForced;      // ran after waiting HEAP_BUDGET_WAIT_MS
    uint32_t heavyWaitMaxMs;
};
HeapBudgetStats heapBudgetStats();

/// One web reply's unit. Movable, so the response object can own it: a
/// member takes it by move (a default-constructed member would try to take
/// a second one).
/// ok() is false when no unit was free: answer 503. With the budget off it
/// is always true and counts nothing.
class WebUnit {
public:
    WebUnit();
    ~WebUnit() { release(); }
    WebUnit(WebUnit&& o) noexcept : _ok(o._ok), _counted(o._counted) { o._counted = false; }
    WebUnit& operator=(WebUnit&& o) noexcept {
        if (this != &o) { release(); _ok = o._ok; _counted = o._counted; o._counted = false; }
        return *this;
    }
    WebUnit(const WebUnit&) = delete;
    WebUnit& operator=(const WebUnit&) = delete;
    bool ok() const { return _ok; }
    void release();
    /// A unit if there is one, else an ok() one that is not counted: for a
    /// reply to a request that has already changed state (a POST that saved,
    /// toasts that were taken). A busy answer there reports a failure for a
    /// change that was made, and the page does not ask again. Its reply is
    /// still built in blocks and refused if the heap has none.
    static WebUnit orBypass();
private:
    bool _ok = false;
    bool _counted = false;
};

/// A heavy job's units for its scope. Blocks the calling task (never the
/// web server's) for up to HEAP_BUDGET_WAIT_MS.
class HeavyLease {
public:
    HeavyLease();
    ~HeavyLease();
    HeavyLease(const HeavyLease&) = delete;
    HeavyLease& operator=(const HeavyLease&) = delete;
private:
    bool _held = false;
};
