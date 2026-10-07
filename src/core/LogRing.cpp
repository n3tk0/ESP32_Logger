#include "LogRing.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

namespace {

constexpr uint32_t MAGIC = 0x4C4F4731;   // "LOG1"; another size is another layout

struct RtcLog {
    uint32_t magic;
    uint32_t total;                 // bytes ever written this boot
    uint16_t prevLen;
    char     live[LOG_RING_BYTES];  // live[p % LOG_RING_BYTES] holds byte p
    char     prev[LOG_RING_PREV];   // the previous boot's tail, from a line start
};

// Not zeroed at boot: that is the point. Validated by logRingBegin().
RTC_NOINIT_ATTR RtcLog s_log;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool         s_ready = false;       // false until logRingBegin(): serial only

const char HEAD_PREV[] = "=== previous boot (tail) ===\n";
const char HEAD_LIVE[] = "=== this boot ===\n";
constexpr size_t HEAD_PREV_LEN = sizeof(HEAD_PREV) - 1;
constexpr size_t HEAD_LIVE_LEN = sizeof(HEAD_LIVE) - 1;

void ringPut(const uint8_t* p, size_t n) {
    if (!s_ready || !n) return;
    portENTER_CRITICAL(&s_mux);
    if (n > LOG_RING_BYTES) {   // only the newest part fits
        s_log.total += n - LOG_RING_BYTES;
        p += n - LOG_RING_BYTES;
        n  = LOG_RING_BYTES;
    }
    const size_t at    = s_log.total % LOG_RING_BYTES;
    const size_t first = n < LOG_RING_BYTES - at ? n : LOG_RING_BYTES - at;
    memcpy(s_log.live + at, p, first);
    memcpy(s_log.live, p + first, n - first);
    s_log.total += n;
    portEXIT_CRITICAL(&s_mux);
}

} // namespace

LogPrint Log;

size_t LogPrint::write(uint8_t c) {
    Serial.write(c);
    ringPut(&c, 1);
    return 1;
}

size_t LogPrint::write(const uint8_t* buf, size_t n) {
    Serial.write(buf, n);
    ringPut(buf, n);
    return n;
}

void logRingBegin() {
    const esp_reset_reason_t why = esp_reset_reason();
    const bool kept = s_log.magic == MAGIC && why != ESP_RST_POWERON &&
                      why != ESP_RST_BROWNOUT && why != ESP_RST_UNKNOWN;
    s_log.prevLen = 0;
    if (kept) {
        // The previous boot's newest LOG_RING_PREV bytes, from a line start.
        const uint32_t held = s_log.total < LOG_RING_BYTES ? s_log.total : LOG_RING_BYTES;
        uint32_t take = held < LOG_RING_PREV ? held : LOG_RING_PREV;
        uint32_t from = s_log.total - take;
        if (take < held) {   // cut mid-line: skip to the next one
            while (take && s_log.live[from % LOG_RING_BYTES] != '\n') { from++; take--; }
            if (take) { from++; take--; }
        }
        for (uint32_t i = 0; i < take; i++)
            s_log.prev[i] = s_log.live[(from + i) % LOG_RING_BYTES];
        s_log.prevLen = (uint16_t)take;
    }
    s_log.magic = MAGIC;
    s_log.total = 0;
    s_ready = true;
}

LogRingView logRingView() {
    LogRingView v{};
    portENTER_CRITICAL(&s_mux);
    const uint32_t total = s_log.total;
    portEXIT_CRITICAL(&s_mux);
    v.liveLen   = total < LOG_RING_BYTES ? total : LOG_RING_BYTES;
    v.liveStart = total - v.liveLen;
    v.prevLen   = s_ready ? s_log.prevLen : 0;
    v.size      = (v.prevLen ? HEAD_PREV_LEN + v.prevLen : 0) + HEAD_LIVE_LEN + v.liveLen;
    return v;
}

size_t logRingRead(const LogRingView& v, size_t index, uint8_t* out, size_t max) {
    size_t n = 0;
    // The parts in order; `index` and `max` walk across them.
    auto part = [&](const char* src, size_t len, bool ring, uint32_t ringFrom) {
        if (index >= len) { index -= len; return; }
        size_t take = len - index;
        if (take > max - n) take = max - n;
        if (ring) {
            portENTER_CRITICAL(&s_mux);
            for (size_t i = 0; i < take; i++)
                out[n + i] = (uint8_t)s_log.live[(ringFrom + index + i) % LOG_RING_BYTES];
            portEXIT_CRITICAL(&s_mux);
        } else {
            memcpy(out + n, src + index, take);
        }
        n += take;
        index = 0;
    };
    if (v.prevLen) {
        part(HEAD_PREV, HEAD_PREV_LEN, false, 0);
        if (n < max) part(s_log.prev, v.prevLen, false, 0);
    }
    if (n < max) part(HEAD_LIVE, HEAD_LIVE_LEN, false, 0);
    if (n < max) part(nullptr, v.liveLen, true, v.liveStart);
    return n;
}
