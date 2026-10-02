#pragma once
#include <stdint.h>

// ============================================================================
// heartbeatSilentMs() — how long a task's heartbeat has been silent.
//
// The software watchdog (TaskManager::checkHealth) reads millis() once and
// then each task's heartbeat. The tasks run at higher priority than loop(), so
// one can stamp a fresh millis() in between, and its heartbeat is then a few
// milliseconds AFTER `now`. Unsigned `now - hb` turned that into ~4294967 s of
// silence and rebooted a healthy device every 10-20 minutes, naming whichever
// task happened to stamp at that moment.
//
// The difference is taken signed: a heartbeat ahead of `now` is fresh (0), and
// the millis() wrap still works for any real gap (under ~24 days).
// ============================================================================
inline uint32_t heartbeatSilentMs(uint32_t now, uint32_t hb) {
    const int32_t d = (int32_t)(now - hb);
    return d > 0 ? (uint32_t)d : 0u;
}
