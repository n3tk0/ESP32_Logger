#pragma once
// ============================================================================
// src/chaos/ChaosMonkey.h — software-in-the-loop fault injector (Phase 3)
// ============================================================================
//
// PURPOSE
//   Deliberately stresses the firmware's resilience paths in a controlled,
//   REPRODUCIBLE way so CI (Wokwi emulator) can prove the device survives a
//   hostile environment: flaky WiFi, mutex contention, and heap pressure.
//
// SAFETY
//   The ENTIRE module is compiled out unless ENABLE_CHAOS_MONKEY is defined
//   (set only by the dedicated [env:chaos_simulator] PlatformIO build).  A
//   normal production .bin does not contain a single byte of this code.
//
// DETERMINISM (Phase 3 design choice: seeded scenario, not pure esp_random)
//   Fault timing comes from a seeded xorshift PRNG (CHAOS_SEED, logged at
//   start-up).  A red CI run is therefore replayable: rebuild with the same
//   seed and the same fault schedule occurs.  This trades a little "realism"
//   for debuggable, non-flaky CI.
//
// WHAT IT INJECTS (each on its own jittered cadence)
//   1. Network flap     — WiFi.disconnect(); reconnect a few seconds later,
//                         exercising the reconnect / AP-fallback logic.
//   2. Mutex starvation — raw-takes fsMutex OR webDataMutex for ~2.5 s so the
//                         MutexGuard timeout paths (Pillar 1 / R1 / R4) are hit
//                         and must degrade gracefully instead of deadlocking.
//   3. Heap pressure    — holds a ~15 KB block for ~5 s to provoke
//                         fragmentation / OOM handling.
//
// INTEGRATION (see tests/chaos/README.md for the exact snippets)
//   #ifdef ENABLE_CHAOS_MONKEY
//   #  include "src/chaos/ChaosMonkey.h"
//   #endif
//   ... at the end of setup():  CHAOS_MONKEY_BEGIN();
// ============================================================================

#ifdef ENABLE_CHAOS_MONKEY

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "../pipeline/DataPipeline.h"   // fsMutex, webDataMutex (extern)

// Fixed default so CI runs are reproducible; override with -DCHAOS_SEED=NNN.
#ifndef CHAOS_SEED
#  define CHAOS_SEED 0x5eed1234u
#endif
// Total run budget (ms); the task stops injecting after this so the validator
// can observe a clean recovery window before the emulator timeout.
#ifndef CHAOS_DURATION_MS
#  define CHAOS_DURATION_MS 150000u   // 150 s (CI runs the emulator ~180 s)
#endif

class ChaosMonkey {
public:
    static void begin() {
        xTaskCreatePinnedToCore(_run, "chaos", 4096, nullptr,
                                1 /* low prio */, nullptr, 0);
    }

private:
    // --- seeded xorshift32 (reproducible) ---
    static uint32_t _rng() {
        static uint32_t s = (CHAOS_SEED ? CHAOS_SEED : 1u);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    static uint32_t _between(uint32_t lo, uint32_t hi) {
        return lo + (_rng() % (hi - lo + 1));
    }

    static void _starveMutex() {
        // Pick one of the two contended mutexes and hold it raw (no MutexGuard)
        // well beyond the consumers' timeouts so their degraded paths run.
        SemaphoreHandle_t m = (_rng() & 1) ? fsMutex : webDataMutex;
        if (!m) return;
        if (xSemaphoreTake(m, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.println("@CHAOS mutex_starve_start");
            vTaskDelay(pdMS_TO_TICKS(2500));
            xSemaphoreGive(m);
            Serial.println("@CHAOS mutex_starve_end");
        }
    }

    static void _heapPressure() {
        const size_t BLK = 15 * 1024;
        void* p = malloc(BLK);
        Serial.printf("@CHAOS heap_pressure alloc=%s\n", p ? "ok" : "fail");
        if (p) {
            memset(p, 0xA5, BLK);          // touch pages so it can't be lazy
            vTaskDelay(pdMS_TO_TICKS(5000));
            free(p);
            Serial.println("@CHAOS heap_pressure_end");
        }
    }

    static void _networkFlap() {
        Serial.println("@CHAOS wifi_drop");
        WiFi.disconnect(false /*wifioff*/);
        vTaskDelay(pdMS_TO_TICKS(_between(3000, 6000)));
        WiFi.reconnect();
        Serial.println("@CHAOS wifi_reconnect_requested");
    }

    static void _run(void*) {
        Serial.printf("@CHAOS begin seed=0x%08x duration_ms=%u\n",
                      (unsigned)CHAOS_SEED, (unsigned)CHAOS_DURATION_MS);
        const uint32_t t0 = millis();
        while (millis() - t0 < CHAOS_DURATION_MS) {
            // Idle a jittered gap, then fire one randomly-chosen fault.
            vTaskDelay(pdMS_TO_TICKS(_between(8000, 18000)));
            switch (_rng() % 3) {
                case 0: _networkFlap();  break;
                case 1: _starveMutex();  break;
                default: _heapPressure(); break;
            }
        }
        Serial.println("@CHAOS done");
        vTaskDelete(nullptr);
    }
};

#  define CHAOS_MONKEY_BEGIN() ChaosMonkey::begin()
#else
#  define CHAOS_MONKEY_BEGIN() do {} while (0)
#endif // ENABLE_CHAOS_MONKEY
