#pragma once
// ============================================================================
// src/chaos/ChaosTelemetry.h — structured survival telemetry (Phase 3)
// ============================================================================
//
// Emits machine-parseable one-line snapshots so the CI validator can assert
// INVARIANTS (heap floor, recovery, no crash) instead of grepping human log
// text.  Gated by ENABLE_CHAOS_TELEMETRY so a normal build is unaffected; it is
// independent of ENABLE_CHAOS_MONKEY so a plain "boot smoke" run can emit
// telemetry with no fault injection.
//
// Line format (stable contract — keep keys in sync with validate_chaos.py):
//   @TLM {"ms":<uptime>,"heap":<free>,"minheap":<min_free>,"rssi":<dbm>,
//         "wifi":<status_int>,"safe":<0|1>,"reset":<reason_int>}
//
// INTEGRATION (see tests/chaos/README.md)
//   #ifdef ENABLE_CHAOS_TELEMETRY
//   #  include "src/chaos/ChaosTelemetry.h"
//   #endif
//   ... at the end of setup():       CHAOS_TELEMETRY_BEGIN();
//   ... once per loop() iteration:    CHAOS_TELEMETRY_TICK();
// ============================================================================

#ifdef ENABLE_CHAOS_TELEMETRY

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

// Provided by src/core/Globals.cpp; declared here so this header stays light.
extern bool g_safeMode;

#ifndef CHAOS_TLM_PERIOD_MS
#  define CHAOS_TLM_PERIOD_MS 1000u
#endif

class ChaosTelemetry {
public:
    static void begin() {
        // One boot banner so the validator can key off a known start marker and
        // capture the power-on reset reason explicitly.
        Serial.printf("@TLM_BOOT {\"reset\":%d,\"heap\":%u}\n",
                      (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap());
    }

    static void tick() {
        static uint32_t last = 0;
        uint32_t now = millis();
        if (now - last < CHAOS_TLM_PERIOD_MS) return;
        last = now;
        Serial.printf(
            "@TLM {\"ms\":%u,\"heap\":%u,\"minheap\":%u,\"rssi\":%d,"
            "\"wifi\":%d,\"safe\":%d,\"reset\":%d}\n",
            (unsigned)now,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMinFreeHeap(),
            (int)WiFi.RSSI(),
            (int)WiFi.status(),
            g_safeMode ? 1 : 0,
            (int)esp_reset_reason());
    }
};

#  define CHAOS_TELEMETRY_BEGIN() ChaosTelemetry::begin()
#  define CHAOS_TELEMETRY_TICK()  ChaosTelemetry::tick()
#else
#  define CHAOS_TELEMETRY_BEGIN() do {} while (0)
#  define CHAOS_TELEMETRY_TICK()  do {} while (0)
#endif // ENABLE_CHAOS_TELEMETRY
