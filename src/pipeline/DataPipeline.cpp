#include "DataPipeline.h"

// Global queue handles — initialised by TaskManager::init()
QueueHandle_t    sensorQueue   = nullptr;
QueueHandle_t    storageQueue  = nullptr;
QueueHandle_t    exportQueue   = nullptr;
SemaphoreHandle_t webDataMutex = nullptr;
SemaphoreHandle_t configMutex  = nullptr;

// Global web ring buffer
RingBuffer<WEB_RING_SIZE> webRingBuf;
