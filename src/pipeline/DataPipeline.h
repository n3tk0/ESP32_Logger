#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string.h>
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

// ============================================================================
// RingBuffer — SPSC ring buffer (finding #17: proper acquire/release atomics)
// Producer: ProcessingTask (push).  Consumer: WebTask (copyRecent, read-only).
// ============================================================================
template<size_t N>
class RingBuffer {
public:
    void push(const SensorReading& r) {
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
        size_t h     = _head.load(std::memory_order_acquire);
        size_t t     = _tail.load(std::memory_order_relaxed);
        size_t start = (h > N) ? (h - N) : t;
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
        size_t h = _head.load(std::memory_order_relaxed);
        size_t t = _tail.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : 0;
    }

    // Scan backwards for the most recent entry matching sensorId + metric
    bool findLast(const char* sensorId, const char* metric,
                  SensorReading& out) const {
        size_t h = _head.load(std::memory_order_acquire);
        size_t t = _tail.load(std::memory_order_relaxed);
        size_t start = (h > N) ? (h - N) : t;
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
        if (maxOut == 0) return 0;
        size_t h = _head.load(std::memory_order_acquire);
        size_t t = _tail.load(std::memory_order_relaxed);
        size_t start = (h > N) ? (h - N) : t;

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
    SensorReading _buf[N] = {};
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

// Ring buffer for recent readings served by /api/data.
// Size is derived from a 16 KB budget so the comment stays correct as
// SensorReading grows.  With sizeof(SensorReading) ≈ 72 B this yields ~227
// entries — enough for ~30 min at 10 s with 3 sensors.
constexpr size_t WEB_RING_SIZE = (16u * 1024u) / sizeof(SensorReading);
extern RingBuffer<WEB_RING_SIZE> webRingBuf;
