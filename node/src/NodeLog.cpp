#include "NodeLog.h"

#include <stdarg.h>
#include <stdio.h>

// The ring: plain text, each line stamped with the uptime it began at. When
// full the oldest bytes go, so the first line shown may start mid-line.
static char     s_ring[NODE_LOG_RING];
static size_t   s_head    = 0;      ///< where the next byte goes
static size_t   s_len     = 0;      ///< bytes held
static bool     s_lineStart = true;

static void put(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        s_ring[s_head] = p[i];
        s_head = (s_head + 1) % NODE_LOG_RING;
        if (s_len < NODE_LOG_RING) s_len++;
    }
}

void nodeLogP(PGM_P fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf_P(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;   // cut, not lost
    Serial.write((const uint8_t*)line, n);

    // A line may come in pieces ("connecting ..." then " ok"): stamp it once,
    // where it begins.
    const char* p = line;
    const char* end = line + n;
    while (p < end) {
        if (s_lineStart) {
            char ts[14];
            const int k = snprintf(ts, sizeof(ts), "[%lu] ", (unsigned long)(millis() / 1000UL));
            if (k > 0) put(ts, (size_t)k);
            s_lineStart = false;
        }
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* stop = nl ? nl + 1 : end;
        put(p, (size_t)(stop - p));
        if (nl) s_lineStart = true;
        p = stop;
    }
}

void nodeLogEach(void (*chunk)(const char* p, size_t n, void* ctx), void* ctx) {
    if (s_len == 0) return;
    const size_t start = (s_head + NODE_LOG_RING - s_len) % NODE_LOG_RING;
    if (start + s_len <= NODE_LOG_RING) {
        chunk(s_ring + start, s_len, ctx);
    } else {
        const size_t first = NODE_LOG_RING - start;
        chunk(s_ring + start, first, ctx);
        chunk(s_ring, s_len - first, ctx);
    }
}

size_t nodeLogSize() { return s_len; }
