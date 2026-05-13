#include "RateLimiter.h"
#include <ESPAsyncWebServer.h>

// Out-of-line definitions for the static members declared in RateLimiter.h.
// Keeps the header C++14-clean (no `static inline` member initialisers).
uint32_t RateLimiter::_tokens     = RateLimiter::MAX_TOKENS;
uint32_t RateLimiter::_lastRefill = 0;

bool rateLimit429(AsyncWebServerRequest* req) {
    if (RateLimiter::allow()) return false;
    // Tell the client when it can retry; REFILL_MS is in milliseconds, so
    // the best we can promise at 1-sec resolution is "in a second".
    AsyncWebServerResponse* resp = req->beginResponse(
        429, "application/json",
        "{\"ok\":false,\"error\":\"rate_limited\"}");
    resp->addHeader("Retry-After", "1");
    req->send(resp);
    return true;
}
