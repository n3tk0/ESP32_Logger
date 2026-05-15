// ============================================================================
// src/web/RequireAuth.h
//
// Unified mutating-handler authorization preamble.
//
// REFACTORING_GUIDELINES Pillar 2.1 requires every mutating HTTP handler to
// begin with EXACTLY ONE statement:
//
//     if (!requireMutatingAuth(req)) return;
//
// No handler may call rateLimit429() or csrfBlock() directly. This funnels
// all admission control through a single audit point.
//
// Order matters:
//   1. Rate limit FIRST — cheap, rejects floods before we touch CSRF state.
//   2. CSRF check next — requires param parsing.
//   3. (Future) optional WEB_BASIC_AUTH_ENABLED check goes here.
//
// Returns true → caller proceeds with the request.
// Returns false → response has ALREADY been sent (429 or 403); caller must
//                 do nothing further with `req`. Just `return;`.
// ============================================================================
#pragma once

class AsyncWebServerRequest;

bool requireMutatingAuth(AsyncWebServerRequest* req);
