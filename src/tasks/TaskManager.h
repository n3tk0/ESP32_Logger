#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <FS.h>

// Task priorities, stack sizes and queue depths are configured in
// src/setup.h (TASK_PRIO_*, STACK_*, QUEUE_*).
#include "../setup.h"

// ============================================================================
// pipelineNowEpoch() — the one answer to "what time is it" for every task
// that stamps or files a reading.
//
// THREE PLACES USED TO ANSWER THIS, AND NOT THE SAME WAY. SensorTask asked
// the system clock first; SlowSensorTask and StorageTask asked the DS1302
// first. That difference is not cosmetic: nothing disciplines the DS1302 to
// NTP and nothing seeds the system clock from it, so the two drift apart with
// no upper bound, and ProcessingTask judges every reading's timestamp against
// the SYSTEM clock via readingIsBackfilled().
//
// Once the RTC ran two minutes slow, everything stamped from it read as
// history: the blocking sensors — the dust sensors and the anemometer, which
// are exactly the ones on SlowSensorTask — dropped out of the live ring and
// out of alert evaluation with nothing said, and the CSV rows StorageTask
// wrote carried a different clock than the dashboard showed.
//
// Order: the system clock when it is plausible (it is the one that gets
// disciplined, and the one the backfill test uses), then the hardware RTC —
// which is what a device with no network has and where this falls through to
// it exactly as before — then a millis counter, +1 so it can never be 0,
// which SensorTypes.h reserves for "unknown" and LiveAggregator uses as its
// first-flush sentinel.
// ============================================================================
uint32_t pipelineNowEpoch();

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

    // Re-read platform_config.json and refresh the in-memory storageParam
    // fields that StorageTask polls every aggregation tick (currently SDS011
    // humidity correction).  Called from TaskManager::init() at boot and from
    // /api/config/platform after sensorManager.reloadConfig() so live UI edits
    // propagate without a reboot.
    // Caller must hold configMutex.
    static void refreshStorageFromPlatform(fs::FS& fs);

    // Task handles (public for diagnostics / watchdog)
    static TaskHandle_t hSensor;
    static TaskHandle_t hSlowSensor;
    static TaskHandle_t hProcess;
    static TaskHandle_t hStorage;
    static TaskHandle_t hExport;

    // Signals tasks to exit their loops
    static std::atomic<bool> running;

    // Startup latch (AUDIT 2.3 follow-up): on a single-core target the higher-
    // priority sensor/process tasks preempt init() the instant they are created
    // — before `running` is set true at the end of init(). They would observe
    // running==false, print "started"/"stopped" and vTaskDelete themselves,
    // leaving dead handles whose heartbeats never update (false watchdog trips).
    // Tasks block on this gate until init() finishes wiring everything up.
    // shutdown() also opens it (sets it true) so a task still parked at startup
    // observes running==false and exits cleanly instead of waiting out the
    // waitForStart() timeout.
    static std::atomic<bool> startGate;

    // Block the calling task until init() opens the start gate (or asks all
    // tasks to stop). Returns true if the task should run, false if it should
    // exit immediately (init failed / shutdown raced startup).
    static bool waitForStart();
};
