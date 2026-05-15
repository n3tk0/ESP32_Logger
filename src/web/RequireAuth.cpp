// ============================================================================
// src/web/RequireAuth.cpp — see RequireAuth.h for contract.
// ============================================================================
#include "RequireAuth.h"

#include <ESPAsyncWebServer.h>

#include "RateLimiter.h"
#include "CsrfToken.h"

// ---------------------------------------------------------------------------
bool requireMutatingAuth(AsyncWebServerRequest* req) {
    if (req == nullptr) return false;

    // Step 1 — Rate limit. rateLimit429() returns true when the request was
    // BLOCKED (the 429 response is sent by the helper). We invert that here
    // so this function's return semantics are "true = allowed".
    if (rateLimit429(req)) return false;

    // Step 2 — CSRF token check. CsrfToken::require() returns true when the
    // token is valid; on failure it sends the 403 and returns false.
    if (!CsrfToken::require(req)) return false;

    // Step 3 (placeholder) — optional auth checks would chain in here.
    // Today, Basic Auth (if compiled in) is enforced by an AsyncWebHandler
    // gate registered FIRST on the server (WebServer.cpp:323-334) so by the
    // time we reach an actual handler, authentication has already passed.

    return true;
}
