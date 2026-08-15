#include "DataPipeline.h"
#include <Arduino.h>

// Global queue handles — initialised by TaskManager::init()
QueueHandle_t    sensorQueue   = nullptr;
QueueHandle_t    storageQueue  = nullptr;
QueueHandle_t    exportQueue   = nullptr;
SemaphoreHandle_t webDataMutex = nullptr;
SemaphoreHandle_t configMutex  = nullptr;
SemaphoreHandle_t wireMutex    = nullptr;
SemaphoreHandle_t fsMutex      = nullptr;

// Queue drop counter (incremented on xQueueSend failure)
volatile uint32_t g_queueDrops = 0;
// Ring push drop counter (incremented when webDataMutex times out)
std::atomic<uint32_t> g_ringPushDrops{0};

// Task heartbeat timestamps (C4)
volatile uint32_t g_taskHeartbeat[TASK_COUNT] = {};

// Global web ring buffer
RingBuffer webRingBuf;

// ---------------------------------------------------------------------------
bool webRingBufInit() {
    size_t bytes     = WEB_RING_BYTES_INTERNAL;
    bool   wantPsram = false;

#if LOGGER_PSRAM_AVAILABLE
    const size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (freePsram > 0) {
        // Cap by both the fixed budget and a share of what is actually there,
        // so a 2 MB part is not drained by a budget written for an 8 MB one.
        const size_t byShare = (freePsram / 100u) * WEB_RING_PSRAM_MAX_PCT;
        size_t want = (size_t)WEB_RING_BYTES_PSRAM;
        if (want > byShare) want = byShare;
        if (want > WEB_RING_BYTES_INTERNAL) {
            bytes     = want;
            wantPsram = true;
        }
        Serial.printf("[RingBuf] PSRAM free %u KB\n", (unsigned)(freePsram / 1024));
    } else {
        Serial.println("[RingBuf] PSRAM enabled in build but none reported — "
                       "check board_build.arduino.memory_type (octal parts need qio_opi)");
    }
#endif

    size_t capacity = bytes / sizeof(SensorReading);
    if (capacity == 0) capacity = 1;

    if (!webRingBuf.begin(capacity, wantPsram)) {
        // Both the PSRAM attempt and begin()'s own internal fallback failed at
        // this size. Retry explicitly at the small budget: a 4 MB request
        // failing says nothing about whether 16 KB would.
        Serial.println("[RingBuf] allocation failed — retrying at internal budget");
        capacity = WEB_RING_BYTES_INTERNAL / sizeof(SensorReading);
        if (!webRingBuf.begin(capacity, false)) {
            Serial.println("[RingBuf] FAILED — /api/data will report no history");
            return false;
        }
    }

    Serial.printf("[RingBuf] %u entries (%u KB) in %s\n",
                  (unsigned)webRingBuf.capacity(),
                  (unsigned)((webRingBuf.capacity() * sizeof(SensorReading)) / 1024),
                  webRingBuf.isPsram() ? "PSRAM" : "internal RAM");
    return true;
}
