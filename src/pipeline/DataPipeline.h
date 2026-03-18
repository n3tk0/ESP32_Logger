#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
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

// ============================================================================
// RingBuffer — lock-free single-producer single-consumer ring buffer
// Used to make recent readings available to WebTask without blocking pipeline.
// ============================================================================
template<size_t N>
class RingBuffer {
public:
    void push(const SensorReading& r) {
        _buf[_head % N] = r;
        _head++;
        if (_head - _tail > N) _tail = _head - N;
    }

    size_t copyRecent(SensorReading* out, size_t maxOut,
                      uint32_t fromTs = 0) const
    {
        size_t start  = (_head > N) ? _head - N : _tail;
        size_t copied = 0;
        for (size_t i = start; i < _head && copied < maxOut; i++) {
            const SensorReading& r = _buf[i % N];
            if (r.timestamp >= fromTs) {
                out[copied++] = r;
            }
        }
        return copied;
    }

    size_t size() const {
        return (_head >= _tail) ? (_head - _tail) : 0;
    }

private:
    SensorReading _buf[N] = {};
    volatile size_t _head = 0;
    volatile size_t _tail = 0;
};

// Global ring buffer: 1000 readings per metric is overkill for ESP32-C3.
// Use 500 mixed readings — enough for ~1.5h at 10s intervals with 3 sensors.
constexpr size_t WEB_RING_SIZE = 500;
extern RingBuffer<WEB_RING_SIZE> webRingBuf;
