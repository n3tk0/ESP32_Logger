#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string.h>
#include <stdlib.h>   // malloc/free for the runtime-sized ring
#include <new>        // placement new
#include "../core/SensorTypes.h"

// ============================================================================
// DataPipeline — FreeRTOS queue handles and synchronisation primitives
//
// Queues are created once in TaskManager::init() and referenced everywhere
// via these externs.  All queues carry SensorReading items.
// ============================================================================

// Inter-task queues
extern QueueHandle_t    sensorQueue;    // SensorTask  → ProcessingTask
extern QueueHandle_t    storageQueue;   // ProcessingTask → StorageTask
extern QueueHandle_t    exportQueue;    // ProcessingTask → ExportTask

// Mutexes
extern SemaphoreHandle_t webDataMutex;  // webRingBuf access (Web ↔ Processing)
extern SemaphoreHandle_t configMutex;   // platform_config.json reload guard
extern SemaphoreHandle_t wireMutex;     // I2C Wire bus serialisation (#14)
extern SemaphoreHandle_t fsMutex;       // LittleFS write serialisation (FS1)

// Drop counter — incremented whenever a queue send fails (finding #3)
extern volatile uint32_t g_queueDrops;
// Drop counter — incremented when webRingBuf push is skipped due to mutex contention
extern std::atomic<uint32_t> g_ringPushDrops;

// Task health heartbeat (C4) — each task writes millis() here every loop
enum TaskIndex : uint8_t {
    TASK_IDX_SENSOR      = 0,
    TASK_IDX_SLOW_SENSOR = 1,
    TASK_IDX_PROCESS     = 2,
    TASK_IDX_STORAGE     = 3,
    TASK_IDX_EXPORT      = 4,
    TASK_COUNT           = 5
};
extern volatile uint32_t g_taskHeartbeat[TASK_COUNT];

// PSRAM support is compiled in only when the board actually has it AND the
// toolchain provides the allocator header.  __has_include keeps the host unit
// tests (which build this header against tests/host/shims) on the plain-malloc
// path without needing a separate mock.
#if defined(BOARD_HAS_PSRAM) && defined(__has_include)
#  if __has_include(<esp_heap_caps.h>)
#    include <esp_heap_caps.h>
#    define LOGGER_PSRAM_AVAILABLE 1
#  endif
#endif
#ifndef LOGGER_PSRAM_AVAILABLE
#  define LOGGER_PSRAM_AVAILABLE 0
#endif

// ---------------------------------------------------------------------------
// Backward-scan limits for the two lookup helpers.
//
// findLast() and collectMetricSeries() walk back from the newest entry until
// they find what they need. When the metric is ABSENT from the ring — a sensor
// stuck on QUALITY_ERROR is never pushed at all (ProcessingTask) — they walk
// the whole thing.
//
// That was self-limiting while capacity was ~227. It is not any more: /api/sensors
// calls findLast() once per metric (up to 16 sensors x 8 metrics = 128 calls)
// and collectMetricSeries() once per sensor, all inside ONE webDataMutex hold —
// the same mutex ProcessingTask takes with a 5 ms timeout before every push,
// counting failures as g_ringPushDrops. An unbounded scan over a 58 000-entry
// PSRAM ring would turn a missing metric into dropped readings.
//
// The limits below keep the worst case near the old cost (~29 000 iterations):
//   128 x 256 + 16 x 2048 = ~66 000 strcmp pairs.
// ---------------------------------------------------------------------------

// findLast: the newest value for a metric sits within roughly one read cycle
// of the head (~19 entries for a 5-sensor build), so 256 is already generous
// — and it exceeds the old whole-ring capacity, so behaviour on the internal
// budget is unchanged.
constexpr size_t RING_SCAN_LIMIT_LAST = 256;

// collectMetricSeries: needs SPARK_MAX (32) samples of ONE metric, which are
// interleaved with every other metric — ~32 x 19 = 608 entries for a 5-sensor
// build. 2048 leaves headroom for denser configurations while staying bounded.
constexpr size_t RING_SCAN_LIMIT_SERIES = 2048;

// ============================================================================
// RingBuffer — SPSC ring buffer (finding #17: proper acquire/release atomics)
// Producer: ProcessingTask (push).  Consumer: WebTask (copyRecent, read-only).
//
// Capacity is a RUNTIME property, set once by begin().  It used to be a
// template parameter backed by a fixed array, which pinned the buffer to
// internal SRAM and to a size the ESP32-C3 could afford.  That mattered more
// than it looks: FS-backed history is not wired up yet (see ApiHandlers —
// "FS query disabled until the wide-CSV reader ships"), so this ring is the
// ONLY source of chart data.  Its depth is literally how far back the
// dashboard can see.
//
// At the old 16 KB budget that was ~227 entries.  A build emitting ~19 metrics
// on a 10 s cadence fills that in about two minutes.  On a board with PSRAM the
// same ring can hold hours instead, which is the entire point of allocating it
// off-chip.
//
// begin() MUST be called before any push/read.  Until it is, every method is a
// safe no-op — the accessors are reachable from the web task from the moment
// the server is up, which can precede pipeline bring-up.
//
// PSRAM caveat: this buffer is touched from tasks only.  It must never be read
// or written from an ISR (the project has IRAM_ATTR handlers for flow, rain and
// wind) — PSRAM is unreachable whenever the flash cache is disabled.
// ============================================================================
class RingBuffer {
public:
    RingBuffer() = default;
    ~RingBuffer() { _release(); }

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ------------------------------------------------------------------
    // begin()
    //   Allocates storage for `capacity` readings.  When `preferPsram` is set
    //   and PSRAM is present, the allocation is attempted there first and
    //   falls back to internal heap on failure.
    //
    //   Returns false if no allocation succeeded; the buffer then stays in its
    //   safe no-op state rather than half-initialised.
    //
    //   Calling begin() twice releases the previous allocation.  Not safe to
    //   call while producers or consumers are running.
    // ------------------------------------------------------------------
    bool begin(size_t capacity, bool preferPsram = true) {
        _release();
        if (capacity == 0) return false;

        const size_t bytes = capacity * sizeof(SensorReading);

#if LOGGER_PSRAM_AVAILABLE
        if (preferPsram) {
            _buf = (SensorReading*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
            if (_buf) _inPsram = true;
        }
#else
        (void)preferPsram;
#endif
        if (!_buf) _buf = (SensorReading*)malloc(bytes);
        if (!_buf) return false;

        // Placement-new, not memset: SensorReading has a user-provided default
        // constructor, so raw malloc'd bytes are not yet a constructed object.
        // The constructor does happen to zero-fill, but reaching that result by
        // memset over a non-trivial type is exactly what -Wclass-memaccess
        // objects to — and it would silently stop being equivalent the moment
        // the struct gains a member that needs real initialisation.
        for (size_t i = 0; i < capacity; i++) new (&_buf[i]) SensorReading();

        _cap = capacity;
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
        return true;
    }

    size_t capacity() const { return _cap; }
    bool   isPsram()  const { return _inPsram; }

    void push(const SensorReading& r) {
        if (!_buf) return;
        const size_t N = _cap;
        // R14 / AUDIT 12.12: ordering matters across the SPSC boundary.
        //  1. write the data slot
        //  2. publish _head with release  → reader's acquire load of
        //     _head synchronises-with this store, so when a reader sees
        //     the new head it ALSO sees the data write that precedes it
        //  3. update _tail (also release) AFTER the head publish — a
        //     reader observing the new _tail before the new _head could
        //     otherwise compute a `start` index that points at the slot
        //     mid-write and read torn data
        size_t h = _head.load(std::memory_order_relaxed);
        size_t newH = h + 1;
        _buf[h % N] = r;                                          // 1
        _head.store(newH, std::memory_order_release);             // 2
        if (newH - _tail.load(std::memory_order_relaxed) > N) {   // 3 — full?
            _tail.store(newH - N, std::memory_order_release);
        }
    }

    size_t copyRecent(SensorReading* out, size_t maxOut,
                      uint32_t fromTs = 0) const
    {
        if (!_buf) return 0;
        const size_t N = _cap;
        size_t h      = _head.load(std::memory_order_acquire);
        size_t t      = _tail.load(std::memory_order_relaxed);
        size_t oldest = (h > N) ? (h - N) : t;

        // Anchor the window to the NEWEST end of the ring.
        //
        // Scanning forward from `oldest` and stopping once maxOut entries are
        // copied returns the OLDEST maxOut entries — the opposite of what the
        // name promises. That was invisible while capacity (227) was smaller
        // than every caller's maxOut (200-500), so the whole ring always fit.
        // Once the ring can hold tens of thousands of entries it would mean
        // /api/latest serving readings hours out of date as "latest".
        size_t start = oldest;
        if ((h - oldest) > maxOut) start = h - maxOut;

        size_t copied = 0;
        for (size_t i = start; i < h && copied < maxOut; i++) {
            const SensorReading& entry = _buf[i % N];
            if (entry.timestamp >= fromTs) {
                out[copied++] = entry;
            }
        }
        return copied;
    }

    size_t size() const {
        if (!_buf) return 0;
        size_t h = _head.load(std::memory_order_relaxed);
        size_t t = _tail.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : 0;
    }

    // Scan backwards for the most recent entry matching sensorId + metric
    bool findLast(const char* sensorId, const char* metric,
                  SensorReading& out) const {
        if (!_buf) return false;
        const size_t N = _cap;
        size_t h = _head.load(std::memory_order_acquire);
        size_t t = _tail.load(std::memory_order_relaxed);
        size_t start = (h > N) ? (h - N) : t;
        // Bounded: see RING_SCAN_LIMIT_LAST. A metric older than this many
        // entries is reported as absent, which is what the freshness UI wants
        // anyway — and the bound exceeds the whole internal-budget ring, so
        // nothing changes on a board without PSRAM.
        if ((h - start) > RING_SCAN_LIMIT_LAST) start = h - RING_SCAN_LIMIT_LAST;
        for (size_t i = h; i > start; ) {
            --i;
            const SensorReading& e = _buf[i % N];
            if (strcmp(e.sensorId, sensorId) == 0 &&
                strcmp(e.metric, metric) == 0) {
                out = e;
                return true;
            }
        }
        return false;
    }

    // Collect up to maxOut most-recent values for sensorId+metric in
    // chronological order (oldest → newest).  Used to render per-card
    // sparklines without a separate endpoint.  Returns the number written.
    size_t collectMetricSeries(const char* sensorId, const char* metric,
                                float* out, size_t maxOut) const {
        if (maxOut == 0 || !_buf) return 0;
        const size_t N = _cap;
        size_t h = _head.load(std::memory_order_acquire);
        size_t t = _tail.load(std::memory_order_relaxed);
        size_t start = (h > N) ? (h - N) : t;
        // Bounded: see RING_SCAN_LIMIT_SERIES. Caps the cost when the metric
        // is absent; a sparkline simply comes back shorter.
        if ((h - start) > RING_SCAN_LIMIT_SERIES) start = h - RING_SCAN_LIMIT_SERIES;

        // Walk backward, append to a temp at decreasing indices so the
        // final compaction yields oldest → newest with one memmove.
        size_t count = 0;
        for (size_t i = h; i > start && count < maxOut; ) {
            --i;
            const SensorReading& e = _buf[i % N];
            if (strcmp(e.sensorId, sensorId) == 0 &&
                strcmp(e.metric, metric) == 0) {
                out[maxOut - 1 - count] = e.value;
                count++;
            }
        }
        if (count < maxOut && count > 0) {
            // Source [maxOut-count .. maxOut-1] overlaps dest [0 .. count-1] when
            // count > maxOut/2 — memmove handles the overlap correctly.
            memmove(out, out + maxOut - count, count * sizeof(float));
        }
        return count;
    }

private:
    void _release() {
        // SensorReading is trivially destructible (no user destructor, all
        // members are scalars/arrays), so the placement-new'd elements need no
        // explicit destructor calls before the storage goes back.
        if (_buf) { free(_buf); _buf = nullptr; }
        _cap     = 0;
        _inPsram = false;
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

    // heap_caps_malloc'd PSRAM and plain malloc'd internal RAM are both
    // released with free() on ESP-IDF, so one path covers each case.
    SensorReading* _buf     = nullptr;
    size_t         _cap     = 0;
    bool           _inPsram = false;
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

// ---------------------------------------------------------------------------
// Ring-buffer sizing budgets.  Both are byte budgets rather than entry counts
// so the arithmetic stays correct as SensorReading grows.
// ---------------------------------------------------------------------------

// Internal-SRAM fallback: what the buffer gets with no PSRAM.  Unchanged from
// the pre-PSRAM behaviour (~227 entries at sizeof(SensorReading) ≈ 72 B), so
// the ESP32-C3 targets keep exactly the footprint they were tuned for.
constexpr size_t WEB_RING_BYTES_INTERNAL = 16u * 1024u;

// PSRAM budget.  4 MB of an 8 MB part is ~58 000 entries — roughly 8 hours for
// a build emitting ~19 metrics every 10 s.  Deliberately not the whole chip:
// PSRAM is a general allocator here, and leaving half free keeps room for
// anything else that wants it (large JSON documents, future FS caches) rather
// than making this one consumer the reason an unrelated allocation fails.
#ifndef WEB_RING_BYTES_PSRAM
#  define WEB_RING_BYTES_PSRAM (4u * 1024u * 1024u)
#endif

// Never claim more than this share of the PSRAM actually reported at boot —
// protects the 2 MB and 4 MB variants of the same board from having the whole
// chip swallowed by the ring.
constexpr uint8_t WEB_RING_PSRAM_MAX_PCT = 50;

// Global web ring buffer.  Sized and allocated by webRingBufInit() at boot;
// every accessor is a safe no-op until then.
extern RingBuffer webRingBuf;

// ---------------------------------------------------------------------------
// webRingBufInit()
//   Chooses a capacity and allocates the ring.  Call once from setup(), before
//   the pipeline tasks start and before the web server can serve /api/data.
//
//   With PSRAM: min(WEB_RING_BYTES_PSRAM, WEB_RING_PSRAM_MAX_PCT % of free
//   PSRAM).  Without, or if the PSRAM allocation fails: the internal budget.
//   Logs the outcome — capacity, byte size and which memory it landed in.
//
//   Returns false only if even the internal fallback could not be allocated,
//   which leaves the dashboard without history but does not stop the logger.
// ---------------------------------------------------------------------------
bool webRingBufInit();
