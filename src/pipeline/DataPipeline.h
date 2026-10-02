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

// ---------------------------------------------------------------------------
// rtcMutex — the DS1302's three-wire bus.
//
// WHY THIS EXISTS. The DS1302 is bit-banged: ThreeWire drives CE/SCLK and
// turns IO around by hand, one edge at a time, with no peripheral and no
// arbitration. Five different contexts reach it — pipelineNowEpoch() from
// SensorTask, SlowSensorTask and StorageTask, the deferred /set_time write
// and the boot-count backup from loop(), getRtcDateTimeString() and
// /api/diag from the AsyncTCP worker, and the NTP sync's SetDateTime — and
// none of them used to take anything. Two interleaved transactions do not
// fail cleanly: they shift each other's bits, so a read returns a plausible
// wrong time (BCD garbage often decodes in range) and a read landing inside
// a SetMemory/SetDateTime can write a register the caller never addressed.
//
// HOLD IT FOR THE TRANSACTION AND NOTHING MORE. It is the innermost lock in
// this firmware: no other mutex may be taken while it is held, which is why
// backupBootCount() releases it before atomicWrite() takes fsMutex. Nothing
// blocks on it for longer than one DS1302 exchange (~1 ms) except the
// deliberate unprotect/write/protect sequences, which are ~120 ms.
extern SemaphoreHandle_t rtcMutex;      // DS1302 three-wire bus serialisation

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

// ---------------------------------------------------------------------------
// Compact ring storage (ESP32-C3 without PSRAM).
//
// A SensorReading is 72 B, and 57 of them are four strings (sensor id, type,
// metric, unit) that repeat in every reading of the same metric. In compact
// mode the ring keeps 12 B per reading — timestamp, value, quality and the
// index of its strings in a small table of RING_KEYS distinct combinations —
// and rebuilds the full SensorReading on the way out. The API is the same;
// only the storage changes. The C3's ~227 readings drop from 16 KB to ~5.5 KB.
//
// A table entry is reused once no reading in the ring refers to it. A reading
// whose combination finds no room (more than RING_KEYS distinct metrics in
// one window) is not stored and is counted in keyDrops().
//
// Override with -DRING_COMPACT=0/1; the host tests build both.
// ---------------------------------------------------------------------------
#ifndef RING_COMPACT
#  if defined(CONFIG_IDF_TARGET_ESP32C3) && !LOGGER_PSRAM_AVAILABLE
#    define RING_COMPACT 1
#  else
#    define RING_COMPACT 0
#  endif
#endif
#ifndef RING_KEYS
#  define RING_KEYS 48
#endif
static_assert(RING_KEYS >= 1 && RING_KEYS <= 255, "RING_KEYS must fit the uint8_t key index");

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
//
// Compact mode keeps the same contract: push() writes a new table entry before
// it publishes _head, so a reader that acquires _head sees the strings too. An
// entry is only reused once no reading in the ring refers to it, so the only
// new tear is the one an overwritten slot already had — a reader still on the
// evicted reading. In the firmware every access holds webDataMutex anyway.
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

#if RING_COMPACT
        (void)preferPsram;
        // One block: the key table first (its size is a multiple of 4, so the
        // slots after it stay aligned), then the slots. Both are plain data.
        void* p = malloc(capacity * sizeof(Slot) + RING_KEYS * sizeof(Key));
        if (!p) return false;
        memset(p, 0, capacity * sizeof(Slot) + RING_KEYS * sizeof(Key));
        _keys  = static_cast<Key*>(p);
        _slots = reinterpret_cast<Slot*>(_keys + RING_KEYS);
#else
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
#endif

        _cap = capacity;
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
        return true;
    }

    size_t capacity() const { return _cap; }
    bool   isPsram()  const { return _inPsram; }

    // Bytes the ring holds on the heap (slots plus, compact, the key table).
    size_t bytes() const {
        if (!_cap) return 0;
#if RING_COMPACT
        return _cap * sizeof(Slot) + RING_KEYS * sizeof(Key);
#else
        return _cap * sizeof(SensorReading);
#endif
    }

    // Readings not stored because the key table was full (compact only).
    uint32_t keyDrops() const { return _keyDrops; }

    void push(const SensorReading& r) {
        if (!_cap) return;
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
#if RING_COMPACT
        // The slot being written holds the oldest reading once the ring is
        // full; that one is evicted, so its key loses a reference.
        if (!_put(h % N, r, (h - _tail.load(std::memory_order_relaxed)) >= N))
            return;                                               // 1
#else
        _buf[h % N] = r;                                          // 1
#endif
        _head.store(newH, std::memory_order_release);             // 2
        if (newH - _tail.load(std::memory_order_relaxed) > N) {   // 3 — full?
            _tail.store(newH - N, std::memory_order_release);
        }
    }

    size_t copyRecent(SensorReading* out, size_t maxOut,
                      uint32_t fromTs = 0) const
    {
        if (!_cap) return 0;
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
            if (_ts(i % N) >= fromTs) _get(i % N, out[copied++]);
        }
        return copied;
    }

    // The same window as copyRecent(out, maxScan, fromTs) — the newest
    // maxScan entries — but only the readings in [fromTs, toTs] of sensorId
    // and metric (nullptr = any). count = true counts them and writes
    // nothing; otherwise the newest maxOut of them go to `out`, oldest first.
    // So a caller counts first and allocates what it will copy, instead of a
    // buffer for the whole window: 300 readings are 21 KB of heap, one
    // sensor's share of them usually a few hundred bytes.
    size_t copyMatching(SensorReading* out, size_t maxOut, size_t maxScan,
                        uint32_t fromTs, uint32_t toTs,
                        const char* sensorId, const char* metric,
                        bool count = false) const
    {
        if (!_cap || (!count && (!out || maxOut == 0))) return 0;
        const size_t N = _cap;
        size_t h      = _head.load(std::memory_order_acquire);
        size_t t      = _tail.load(std::memory_order_relaxed);
        size_t oldest = (h > N) ? (h - N) : t;
        size_t start  = oldest;
        if ((h - oldest) > maxScan) start = h - maxScan;

        size_t n = 0;
        for (size_t i = h; i > start && (count || n < maxOut); ) {
            --i;
            const size_t s  = i % N;
            const uint32_t ts = _ts(s);
            if (ts < fromTs || ts > toTs) continue;
            if (!_is(s, sensorId, metric)) continue;
            if (!count) _get(s, out[maxOut - 1 - n]);
            n++;
        }
        if (!count && n < maxOut && n > 0)
            memmove(out, out + maxOut - n, n * sizeof(SensorReading));
        return n;
    }

    // The newest reading of each (sensorId, metric) among the newest maxScan
    // entries, newest first, at most maxOut of them. No copy of the window.
    size_t latestPerMetric(SensorReading* out, size_t maxOut, size_t maxScan) const {
        if (!_cap || !out || maxOut == 0) return 0;
        const size_t N = _cap;
        size_t h      = _head.load(std::memory_order_acquire);
        size_t t      = _tail.load(std::memory_order_relaxed);
        size_t oldest = (h > N) ? (h - N) : t;
        size_t start  = oldest;
        if ((h - oldest) > maxScan) start = h - maxScan;

        size_t n = 0;
        for (size_t i = h; i > start && n < maxOut; ) {
            --i;
            const size_t s = i % N;
            bool seen = false;
            for (size_t j = 0; j < n && !seen; j++)
                seen = _is(s, out[j].sensorId, out[j].metric);
            if (!seen) _get(s, out[n++]);
        }
        return n;
    }

    size_t size() const {
        if (!_cap) return 0;
        size_t h = _head.load(std::memory_order_relaxed);
        size_t t = _tail.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : 0;
    }

    // Scan backwards for the most recent entry matching sensorId + metric
    bool findLast(const char* sensorId, const char* metric,
                  SensorReading& out) const {
        if (!_cap) return false;
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
            if (_is(i % N, sensorId, metric)) {
                _get(i % N, out);
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
        if (maxOut == 0 || !_cap) return 0;
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
            if (_is(i % N, sensorId, metric)) {
                out[maxOut - 1 - count] = _val(i % N);
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
#if RING_COMPACT
    // One reading: everything but the strings, which live in _keys.
    struct Slot {
        uint32_t ts;
        float    value;
        uint8_t  key;       // index into _keys
        uint8_t  quality;   // SensorQuality
    };
    // One distinct (sensorId, sensorType, metric, unit), sized like
    // SensorReading's own fields so a round trip is exact.
    struct Key {
        char     sensorId[sizeof(SensorReading::sensorId)];
        char     sensorType[sizeof(SensorReading::sensorType)];
        char     metric[sizeof(SensorReading::metric)];
        char     unit[sizeof(SensorReading::unit)];
        uint16_t refs;      // slots in the ring that point here
    };
    static_assert(sizeof(Key) % 4 == 0, "the slots follow the key table in one block");

    uint32_t _ts(size_t s)  const { return _slots[s].ts; }
    float    _val(size_t s) const { return _slots[s].value; }

    // nullptr matches anything.
    bool _is(size_t s, const char* id, const char* metric) const {
        const Key& k = _keys[_slots[s].key];
        return (!id     || strcmp(k.sensorId, id)     == 0) &&
               (!metric || strcmp(k.metric,   metric) == 0);
    }

    void _get(size_t s, SensorReading& out) const {
        const Slot& sl = _slots[s];
        const Key&  k  = _keys[sl.key];
        out.timestamp = sl.ts;
        memcpy(out.sensorId,   k.sensorId,   sizeof(out.sensorId));
        memcpy(out.sensorType, k.sensorType, sizeof(out.sensorType));
        memcpy(out.metric,     k.metric,     sizeof(out.metric));
        out.value   = sl.value;
        memcpy(out.unit,       k.unit,       sizeof(out.unit));
        out.quality = (SensorQuality)sl.quality;
    }

    // Writes slot `s`. `evict`: it holds a live reading that is being dropped.
    // False (and nothing written) when the key table has no room.
    bool _put(size_t s, const SensorReading& r, bool evict) {
        Slot& sl = _slots[s];
        if (evict && _keys[sl.key].refs) _keys[sl.key].refs--;
        const int k = _keyFor(r);
        if (k < 0) {
            if (evict) _keys[sl.key].refs++;   // the old reading stays
            _keyDrops++;
            return false;
        }
        _keys[k].refs++;
        sl.ts      = r.timestamp;
        sl.value   = r.value;
        sl.key     = (uint8_t)k;
        sl.quality = (uint8_t)r.quality;
        return true;
    }

    // The entry for r's strings: an existing one, else a free one filled in.
    static bool _same(const Key& k, const SensorReading& r) {
        return strcmp(k.sensorId,   r.sensorId)   == 0 &&
               strcmp(k.metric,     r.metric)     == 0 &&
               strcmp(k.sensorType, r.sensorType) == 0 &&
               strcmp(k.unit,       r.unit)       == 0;
    }
    int _keyFor(const SensorReading& r) {
        int freeK = -1;
        for (int k = 0; k < RING_KEYS; k++) {
            if (_same(_keys[k], r)) return k;
            if (freeK < 0 && _keys[k].refs == 0) freeK = k;
        }
        if (freeK < 0) return -1;
        // SensorReading's strings are NUL-terminated within their arrays
        // (make() and the zeroing constructor see to it); copy them whole and
        // pin the terminator anyway, so a table entry is always a C string.
        Key& e = _keys[freeK];
        memcpy(e.sensorId,   r.sensorId,   sizeof(e.sensorId));   e.sensorId[sizeof(e.sensorId) - 1]     = '\0';
        memcpy(e.sensorType, r.sensorType, sizeof(e.sensorType)); e.sensorType[sizeof(e.sensorType) - 1] = '\0';
        memcpy(e.metric,     r.metric,     sizeof(e.metric));     e.metric[sizeof(e.metric) - 1]         = '\0';
        memcpy(e.unit,       r.unit,       sizeof(e.unit));       e.unit[sizeof(e.unit) - 1]             = '\0';
        return freeK;
    }
#else
    uint32_t _ts(size_t s)  const { return _buf[s].timestamp; }
    float    _val(size_t s) const { return _buf[s].value; }

    // nullptr matches anything.
    bool _is(size_t s, const char* id, const char* metric) const {
        const SensorReading& e = _buf[s];
        return (!id     || strcmp(e.sensorId, id)     == 0) &&
               (!metric || strcmp(e.metric,   metric) == 0);
    }

    void _get(size_t s, SensorReading& out) const { out = _buf[s]; }
#endif

    void _release() {
#if RING_COMPACT
        // _slots lives in the same block as _keys.
        if (_keys) { free(_keys); _keys = nullptr; _slots = nullptr; }
#else
        // SensorReading is trivially destructible (no user destructor, all
        // members are scalars/arrays), so the placement-new'd elements need no
        // explicit destructor calls before the storage goes back.
        if (_buf) { free(_buf); _buf = nullptr; }
#endif
        _cap      = 0;
        _inPsram  = false;
        _keyDrops = 0;
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

#if RING_COMPACT
    Key*           _keys    = nullptr;
    Slot*          _slots   = nullptr;
#else
    // heap_caps_malloc'd PSRAM and plain malloc'd internal RAM are both
    // released with free() on ESP-IDF, so one path covers each case.
    SensorReading* _buf     = nullptr;
#endif
    size_t         _cap     = 0;
    bool           _inPsram = false;
    uint32_t       _keyDrops = 0;
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

// ---------------------------------------------------------------------------
// Ring-buffer sizing budgets.  Both are byte budgets rather than entry counts
// so the arithmetic stays correct as SensorReading grows.
// ---------------------------------------------------------------------------

// Internal-SRAM fallback: what the buffer gets with no PSRAM.  Unchanged from
// the pre-PSRAM behaviour (~227 entries at sizeof(SensorReading) ≈ 72 B), so
// the ESP32-C3 targets keep exactly the history they were tuned for. With
// RING_COMPACT the same ~227 entries take ~5.5 KB instead (see bytes()).
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
