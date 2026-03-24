#include "DataPipeline.h"

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

// Task heartbeat timestamps (C4)
volatile uint32_t g_taskHeartbeat[TASK_COUNT] = {};

// Global web ring buffer
RingBuffer<WEB_RING_SIZE> webRingBuf;
