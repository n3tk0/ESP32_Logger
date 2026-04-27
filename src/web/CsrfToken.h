#pragma once
#include <Arduino.h>

class AsyncWebServerRequest;

// ============================================================================
// CsrfToken — per-boot 32-hex random token (Pass 7).
//
// On first request the token is materialised from esp_random() (~128 bits
// of entropy from the hardware RNG), then served at GET /api/csrf-token.
// Mutating routes call csrfRequire(req) which compares the `csrf`
// form/query param against the stored token.  Mismatch → 403 +
// {"ok":false,"error":"csrf"}.  ESPAsyncWebServer drops custom request
// headers by default, so we keep the contract param-only.
//
// Per-boot scope is intentionally simple: there's no session state on the
// device, so token freshness is bounded by reboot.  The same token is
// reused across the lifetime of the firmware run, which is fine for a
// local-network device under basic auth.
// ============================================================================
namespace CsrfToken {
    // Returns the per-boot token (32 hex chars), generating it on first call.
    const char* get();

    // True if the request carries a token that matches.  Sends 403 + JSON
    // and returns true ("blocked") if not — caller should `return` straight
    // after.  Mirrors the rateLimit429() shape from RateLimiter.h.
    bool require(AsyncWebServerRequest* req);
}

// Convenience — usage at the top of a mutating handler:
//   if (csrfBlock(req)) return;
static inline bool csrfBlock(AsyncWebServerRequest* req) {
    return !CsrfToken::require(req);
}
