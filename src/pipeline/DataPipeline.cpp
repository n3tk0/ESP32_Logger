#include "DataPipeline.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include <Arduino.h>

// Global queue handles — initialised by TaskManager::init()
QueueHandle_t    sensorQueue   = nullptr;
QueueHandle_t    storageQueue  = nullptr;
QueueHandle_t    exportQueue   = nullptr;
SemaphoreHandle_t webDataMutex = nullptr;
SemaphoreHandle_t configMutex  = nullptr;
SemaphoreHandle_t wireMutex    = nullptr;
SemaphoreHandle_t fsMutex      = nullptr;
SemaphoreHandle_t rtcMutex      = nullptr;

// Queue drop counter (incremented on xQueueSend failure)
volatile uint32_t g_queueDrops = 0;
// Ring push drop counter (incremented when webDataMutex times out)
std::atomic<uint32_t> g_ringPushDrops{0};
std::atomic<uint32_t> g_exportSkips{0};

// Task heartbeat timestamps (C4)
volatile uint32_t g_taskHeartbeat[TASK_COUNT] = {};

// Global web ring buffer
RingBuffer webRingBuf;

// ---------------------------------------------------------------------------
bool webRingBufInit() {
    size_t bytes     = WEB_RING_BYTES_INTERNAL;
    bool   wantPsram = false;

    // The compact layout lives in internal RAM; a build that forces it on a
    // PSRAM board keeps the internal budget rather than asking for 4 MB.
#if LOGGER_PSRAM_AVAILABLE && !RING_COMPACT
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
        Log.printf("[RingBuf] PSRAM free %u KB\n", (unsigned)(freePsram / 1024));
    } else {
        Log.println("[RingBuf] PSRAM enabled in build but none reported — "
                       "check board_build.arduino.memory_type (octal parts need qio_opi)");
    }
#endif

    size_t capacity = bytes / sizeof(SensorReading);
    if (capacity == 0) capacity = 1;

    if (!webRingBuf.begin(capacity, wantPsram)) {
        // Both the PSRAM attempt and begin()'s own internal fallback failed at
        // this size. Retry explicitly at the small budget: a 4 MB request
        // failing says nothing about whether 16 KB would.
        Log.println("[RingBuf] allocation failed — retrying at internal budget");
        capacity = WEB_RING_BYTES_INTERNAL / sizeof(SensorReading);
        if (!webRingBuf.begin(capacity, false)) {
            Log.println("[RingBuf] FAILED — /api/data will report no history");
            return false;
        }
    }

    Log.printf("[RingBuf] %u entries (%u KB%s) in %s\n",
                  (unsigned)webRingBuf.capacity(),
                  (unsigned)(webRingBuf.bytes() / 1024),
                  RING_COMPACT ? ", compact" : "",
                  webRingBuf.isPsram() ? "PSRAM" : "internal RAM");
    return true;
}
