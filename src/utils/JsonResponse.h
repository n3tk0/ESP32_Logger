// ============================================================================
// src/utils/JsonResponse.h
//
// sendJsonResponse — serialize a JsonDocument to an ESPAsyncWebServer
// streaming response and send it, with Content-Type: application/json;
// 503 + Retry-After: 1 when the heap cannot hold the whole reply.
//
// Usage:
//   static void handleApiFoo(AsyncWebServerRequest* req) {
//       JsonDocument doc;
//       doc["ok"] = true;
//       sendJsonResponse(req, doc);
//   }
//
// When NOT to use:
//   - When you need custom response headers (Content-Disposition, etc.) —
//     call beginResponseStream / addHeader / serializeJson / send directly.
//   - When you are streaming chunked JSON built with printf/print instead of
//     a JsonDocument (e.g. /api/data large-buffer path).
// ============================================================================
#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

inline void sendJsonResponse(AsyncWebServerRequest* r, const JsonDocument& doc) {
    // The reply is buffered whole before it goes out, so size the buffer
    // once from measureJson() rather than letting it grow (each growth a
    // realloc while the other page-load requests hold heap) — and check that
    // all of it fit. A document that ran out of heap while being built
    // (overflowed) or a buffer that could not take every byte would go out
    // as a 200 with a cut-short or empty body, which the page cannot parse
    // and does not retry: answer 503 + Retry-After instead, which it does.
    const size_t n = doc.overflowed() ? 0 : measureJson(doc);
    AsyncResponseStream* resp = n ? r->beginResponseStream("application/json", n + 1) : nullptr;
    if (resp && serializeJson(doc, static_cast<Print&>(*resp)) == n) {
        r->send(resp);
        return;
    }
    delete resp;   // never sent: still ours
    AsyncWebServerResponse* busy = r->beginResponse(
        503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    if (!busy) { r->send(503); return; }   // (Pillar 3.1: no silent OOM fallback)
    busy->addHeader("Retry-After", "1");
    r->send(busy);
}
