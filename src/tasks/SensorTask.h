#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// SensorTask — polls all registered ISensor plugins.
//
// Loop:
//   1. Get current timestamp from RTC/NTP
//   2. Call sensorManager.tick(sensorQueue, ts)
//   3. vTaskDelay(10ms)   — yield; queues handle backpressure
//
// Priority: TASK_PRIO_SENSOR (highest among data tasks)
// Stack:    STACK_SENSOR_TASK
// ============================================================================
void sensorTaskFunc(void* param);
