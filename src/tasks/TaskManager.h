#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// Task priorities (higher number = higher priority)
#define TASK_PRIO_SENSOR      3
#define TASK_PRIO_PROCESS     2
#define TASK_PRIO_SLOW_SENSOR 2   // Blocking sensors — same as process, below fast sensor
#define TASK_PRIO_STORAGE     1
#define TASK_PRIO_EXPORT      1

// Stack sizes in bytes (tuned for ESP32-C3, 400KB DRAM total)
#define STACK_SENSOR_TASK      4096
#define STACK_PROCESS_TASK     6144   // LTTB intermediate buffer on stack
#define STACK_SLOW_SENSOR_TASK 4096   // Blocking sensor reads (UART + delay)
#define STACK_STORAGE_TASK     6144   // Two JsonLogger (~1.3KB each) + File I/O
#define STACK_EXPORT_TASK      8192   // WiFi + TLS + JSON serialisation

// Queue depths (items = SensorReading, ~80 bytes each)
#define QUEUE_SENSOR_DEPTH  20
#define QUEUE_STORAGE_DEPTH 32
#define QUEUE_EXPORT_DEPTH  32

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
