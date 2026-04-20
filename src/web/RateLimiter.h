#pragma once
#include <Arduino.h>

class AsyncWebServerRequest;

// ============================================================================
// RateLimiter — global token-bucket for mutating HTTP handlers (Pass 7).
//
// Protects flash + task queues from an authenticated-but-runaway client (or
// a bug in the UI) hammering /save_* / /api/modules/:id writes.  One global
// bucket, not per-IP — single process, tiny device.
//
// Usage at the top of a mutating handler:
//     if (rateLimit429(req)) return;
// ============================================================================
class RateLimiter {
public:
    static constexpr uint32_t REFILL_MS  = 200;    // +1 token every 200 ms
    static constexpr uint32_t MAX_TOKENS = 20;     // 20-request burst

    static bool allow() {
        uint32_t now = millis();
        uint32_t elapsed = now - _lastRefill;
        if (elapsed >= REFILL_MS) {
            uint32_t add = elapsed / REFILL_MS;
            _tokens = (_tokens + add > MAX_TOKENS) ? MAX_TOKENS : (_tokens + add);
            _lastRefill += add * REFILL_MS;
        }
        if (_tokens == 0) return false;
        _tokens--;
        return true;
    }

private:
    static inline uint32_t _tokens     = MAX_TOKENS;
    static inline uint32_t _lastRefill = 0;
};

// Sends a 429 response and returns true if over budget (caller should return
// immediately).  Defined in RateLimiter.cpp so callers don't have to pull in
// ESPAsyncWebServer here.
bool rateLimit429(AsyncWebServerRequest* req);
