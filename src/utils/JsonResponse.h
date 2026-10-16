// ============================================================================
// src/utils/JsonResponse.h
//
// sendJsonResponse — serialize a JsonDocument and send it, with
// Content-Type: application/json; 503 + Retry-After: 1 when the heap cannot
// hold the whole reply.
//
// Usage:
//   static void handleApiFoo(AsyncWebServerRequest* req) {
//       JsonDocument doc;
//       doc["ok"] = true;
//       sendJsonResponse(req, doc);
//   }
//
// Replies built with print/printf (/api/ingest) measure their length first,
// write into a BlockPrint of that length and send it with sendBlockPrint().
//
// NEVER an AsyncResponseStream. Its buffer is one block that grows by a
// realloc per write; when a growth fails, every later byte tries the
// allocation again and logs "Failed to allocate". ArduinoJson writes one byte
// at a time, and a reply that could not grow kept the AsyncTCP task in malloc
// past the 5 s task watchdog: the Overview and the Sensors page reset the C3
// (2026-10-16). A BlockPrint takes 512 B blocks, which a fragmented heap
// still has, stops at the first one it cannot get, and never takes more than
// the length it was given.
// ============================================================================
#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <memory>
#include <new>

#include "../web/BlockBuf.h"
#include "../core/LogRing.h"   // Log

// A reply of exactly `limit` bytes, assembled in blocks. Bytes past the limit
// are counted, not kept: over() says the length worked out beforehand was
// wrong, and the reply must not go out under it.
struct BlockPrint : public Print {
    BlockBuf buf;
    size_t   limit;
    size_t   extra = 0;
    explicit BlockPrint(size_t n) : limit(n) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* p, size_t n) override {
        const size_t left = limit - buf.size();
        if (n > left) { extra += n - left; n = left; }
        return n ? buf.write(p, n) : 0;
    }
    bool over()     const { return extra != 0; }
    bool complete() const { return !buf.failed() && !extra && buf.size() == limit; }
};

// 503 + Retry-After: 1 — the page asks a GET once more after it.
inline void sendBusyResponse(AsyncWebServerRequest* r) {
    AsyncWebServerResponse* busy = r->beginResponse(
        503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    if (!busy) { r->send(503); return; }   // (Pillar 3.1: no silent OOM fallback)
    busy->addHeader("Retry-After", "1");
    r->send(busy);
}

// A BlockPrint that is complete(), sent with its length; false (nothing sent)
// when it is not, or the response itself cannot be had. A length that came
// out wrong is logged with the request's path: that is a bug, not the heap.
inline bool sendBlockPrint(AsyncWebServerRequest* r, std::shared_ptr<BlockPrint> pay,
                           const char* type, int code = 200,
                           const char* attachment = nullptr) {
    if (!pay) return false;
    if (pay->over() || (!pay->buf.failed() && pay->buf.size() != pay->limit)) {
        Log.printf("[web] %s: reply measured %u B, wrote %u B\n", r->url().c_str(),
                   (unsigned)pay->limit, (unsigned)(pay->buf.size() + pay->extra));
        return false;
    }
    if (!pay->complete()) return false;
    AsyncWebServerResponse* res = r->beginResponse(type, pay->limit,
        [pay](uint8_t* out, size_t maxLen, size_t) -> size_t { return pay->buf.read(out, maxLen); });
    if (!res) return false;
    res->setCode(code);
    if (attachment)
        res->addHeader("Content-Disposition",
                       String("attachment; filename=\"") + attachment + "\"");
    r->send(res);
    return true;
}

// `attachment`, when given, is the file name for a Content-Disposition
// header (settings and backup downloads).
inline void sendJsonResponse(AsyncWebServerRequest* r, const JsonDocument& doc,
                             int code = 200, const char* attachment = nullptr) {
    // A document that ran out of heap while being built (overflowed), or a
    // reply that could not be had whole, would go out as a 200 with a
    // cut-short body, which the page cannot parse and does not retry: answer
    // 503 + Retry-After instead, which it does.
    const size_t n = doc.overflowed() ? 0 : measureJson(doc);
    std::shared_ptr<BlockPrint> pay;
    if (n) {
        BlockPrint* p = new (std::nothrow) BlockPrint(n);
        if (p) pay.reset(p);
    }
    if (pay) serializeJson(doc, static_cast<Print&>(*pay));
    if (!sendBlockPrint(r, pay, "application/json", code, attachment))
        sendBusyResponse(r);
}
