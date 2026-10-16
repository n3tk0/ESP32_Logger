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
// Replies built with print/printf (/api/data, /api/ingest) measure their
// length first and ask beginSizedStream() for the buffer, sending
// sendBusyResponse() when it says no.
//
// NEVER call beginResponseStream() without a size here. That buffer grows by
// one realloc per write; when a growth fails the library logs "Failed to
// allocate" for every following write, and ArduinoJson writes one byte at a
// time: a 2 KB reply became 2000 lines on the UART, the AsyncTCP task stalled
// past the 5 s task watchdog, and the Overview reset the C3 (2026-10-16).
// ============================================================================
#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Bytes the largest free block must hold beyond the reply buffer itself.
#ifndef JSON_RESPONSE_HEADROOM
#define JSON_RESPONSE_HEADROOM 1024
#endif

// A stream whose buffer holds n bytes without growing, or nullptr when the
// heap has no block for it. The buffer is one block of n + 2 bytes (cbuf adds
// one), so ask for it only when the largest free block holds it with room to
// spare for the other requests in flight: a page-load burst can leave the
// heap with 20 KB free but no 7 KB piece. Checked first because cbuf's
// constructor does not test its own allocation.
inline AsyncResponseStream* beginSizedStream(AsyncWebServerRequest* r, const char* type, size_t n) {
    if (!n || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < n + 2 + JSON_RESPONSE_HEADROOM)
        return nullptr;
    return r->beginResponseStream(type, n + 1);
}

// 503 + Retry-After: 1 — the page asks a GET once more after it.
inline void sendBusyResponse(AsyncWebServerRequest* r) {
    AsyncWebServerResponse* busy = r->beginResponse(
        503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    if (!busy) { r->send(503); return; }   // (Pillar 3.1: no silent OOM fallback)
    busy->addHeader("Retry-After", "1");
    r->send(busy);
}

// `attachment`, when given, is the file name for a Content-Disposition
// header (settings and backup downloads).
inline void sendJsonResponse(AsyncWebServerRequest* r, const JsonDocument& doc,
                             int code = 200, const char* attachment = nullptr) {
    // The reply is buffered whole before it goes out, so size the buffer
    // once from measureJson() rather than letting it grow — and check that
    // all of it fit. A document that ran out of heap while being built
    // (overflowed) or a buffer that could not take every byte would go out
    // as a 200 with a cut-short or empty body, which the page cannot parse
    // and does not retry: answer 503 + Retry-After instead, which it does.
    const size_t n = doc.overflowed() ? 0 : measureJson(doc);
    AsyncResponseStream* resp = beginSizedStream(r, "application/json", n);
    if (resp && serializeJson(doc, static_cast<Print&>(*resp)) == n) {
        resp->setCode(code);
        if (attachment)
            resp->addHeader("Content-Disposition",
                            String("attachment; filename=\"") + attachment + "\"");
        r->send(resp);
        return;
    }
    delete resp;   // never sent: still ours
    sendBusyResponse(r);
}
