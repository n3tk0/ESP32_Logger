#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// Task priorities, stack sizes and queue depths are configured in
// src/setup.h (TASK_PRIO_*, STACK_*, QUEUE_*).
#include "../setup.h"

// ============================================================================
// TaskManager — creates all FreeRTOS queues, mutexes, and tasks.
// Call init() once from setup() AFTER WiFi, storage, and sensors are ready.
// ============================================================================
class TaskManager {
public:
    // Create queues and start all tasks.
    // fs: active filesystem (LittleFS or SD) for StorageTask.
    static bool init(fs::FS& fs);

    // Graceful shutdown (signal all tasks to stop, wait for idle).
    // Call before deep sleep or factory reset.
    static void shutdown();

    // Software watchdog (C4): returns false if any task is stuck (>30s no heartbeat)
    static bool checkHealth();

    // Task handles (public for diagnostics / watchdog)
    static TaskHandle_t hSensor;
    static TaskHandle_t hSlowSensor;
    static TaskHandle_t hProcess;
    static TaskHandle_t hStorage;
    static TaskHandle_t hExport;

    // Signals tasks to exit their loops
    static volatile bool running;
};
