// ============================================================================
// src/export/Throttle.h — how long an exporter waits after the server said
// "too many requests" (HTTP 429)
//
// A 429 used to be retried like a timeout: four POSTs in about 7 s, then the
// batch went to the spool, and the next minute sent the spool AND the new
// batch. Every retry counted against the same limit, so openSenseMap never
// stopped answering 429 and the spool filled up and dropped data.
//
// Now a 429 ends the send at once (no retry, no spool) and the exporter
// waits: as long as the server's Retry-After asks, but at least a back-off
// that starts at one minute and doubles with every 429 in a row up to
// 15 minutes. One accepted send resets it.
// ============================================================================
#pragma once

#include <stdint.h>

constexpr uint32_t THROTTLE_FIRST_MS = 60000UL;          // 1 min
constexpr uint32_t THROTTLE_MAX_MS   = 15UL * 60000UL;   // 15 min
constexpr uint32_t RETRY_AFTER_MAX_S = 3600UL;           // trust it up to 1 h

/// The back-off after one more 429 in a row: 1, 2, 4, 8, 15, 15 … min.
/// `prevMs` is the previous wait (0 after an accepted send).
inline uint32_t throttleNextBackoffMs(uint32_t prevMs) {
    if (prevMs < THROTTLE_FIRST_MS) return THROTTLE_FIRST_MS;
    if (prevMs >= THROTTLE_MAX_MS / 2) return THROTTLE_MAX_MS;
    return prevMs * 2;
}

/// The wait for this 429: the back-off, or Retry-After when it asks for
/// longer (capped at an hour, so a garbled header cannot silence the exporter
/// for days). `retryAfterS` is 0 when the server sent none.
inline uint32_t throttleWaitMs(uint32_t backoffMs, uint32_t retryAfterS) {
    if (retryAfterS > RETRY_AFTER_MAX_S) retryAfterS = RETRY_AFTER_MAX_S;
    const uint32_t ra = retryAfterS * 1000UL;
    return ra > backoffMs ? ra : backoffMs;
}

/// Retry-After as seconds: "120" → 120. The HTTP-date form, an empty or
/// garbled value → 0 (the back-off alone decides).
inline uint32_t parseRetryAfterS(const char* v) {
    if (!v) return 0;
    while (*v == ' ') v++;
    uint32_t s = 0;
    bool any = false;
    for (; *v >= '0' && *v <= '9'; v++) {
        any = true;
        if (s > RETRY_AFTER_MAX_S) continue;   // already past the cap
        s = s * 10 + (uint32_t)(*v - '0');
    }
    while (*v == ' ') v++;
    return (any && *v == '\0') ? s : 0;
}
