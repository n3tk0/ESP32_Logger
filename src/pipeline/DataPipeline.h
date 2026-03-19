#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
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

// Drop counter — incremented whenever a queue send fails (finding #3)
extern volatile uint32_t g_queueDrops;

// ============================================================================
// RingBuffer — SPSC ring buffer (finding #17: proper acquire/release atomics)
// Producer: ProcessingTask (push).  Consumer: WebTask (copyRecent, read-only).
// ============================================================================
template<size_t N>
class RingBuffer {
public:
    void push(const SensorReading& r) {
        size_t h = _head.load(std::memory_order_relaxed);
        _buf[h % N] = r;
        size_t newH = h + 1;
        // Advance tail if buffer is full (drop oldest)
        if (newH - _tail.load(std::memory_order_relaxed) > N) {
            _tail.store(newH - N, std::memory_order_relaxed);
        }
        _head.store(newH, std::memory_order_release);
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

private:
    SensorReading _buf[N] = {};
    std::atomic<size_t> _head{0};
    std::atomic<size_t> _tail{0};
};

// Global ring buffer: 1000 readings per metric is overkill for ESP32-C3.
// Use 500 mixed readings — enough for ~1.5h at 10s intervals with 3 sensors.
constexpr size_t WEB_RING_SIZE = 500;
extern RingBuffer<WEB_RING_SIZE> webRingBuf;
