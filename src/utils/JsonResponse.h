// ============================================================================
// src/utils/JsonResponse.h
//
// sendJsonResponse — serialize a JsonDocument to an ESPAsyncWebServer
// streaming response and send it, with Content-Type: application/json.
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

inline void sendJsonResponse(AsyncWebServerRequest* r, JsonDocument& doc) {
    AsyncResponseStream* resp = r->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    r->send(resp);
}
