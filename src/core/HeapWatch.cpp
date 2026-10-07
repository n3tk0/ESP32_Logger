#include "HeapWatch.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <string.h>

#include "EventLog.h"
#include "../pipeline/DataPipeline.h"   // g_queueDrops

namespace {

// An activity finished this long ago is still named in a low-heap line: the
// poll runs every 500 ms, and a response is sent after its handler returns.
constexpr uint32_t RECENT_MS        = 5000;
constexpr uint32_t POLL_MS          = 500;
constexpr uint32_t HEAP_STEP_BYTES  = 1024;   // next line only 1 KB lower
constexpr uint8_t  HEAP_LINES_MAX   = 6;      // per boot
constexpr uint32_t LOST_EVERY_MS    = 60000;
constexpr uint8_t  LOST_LINES_MAX   = 10;     // per boot; diag keeps counting

struct Slot {
    char     what[32];
    uint32_t startMs;
    uint32_t endMs;
    bool     active;
};

Slot        s_slots[HA_COUNT];
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

const char* const KIND_NAMES[HA_COUNT] = { "web", "export", "forecast", "alert" };

// Lost readings since the last line, and the place that lost the last one.
uint32_t     s_lostPending = 0;
const char*  s_lostWhere   = nullptr;

uint32_t s_heapLogged = 0;   // low-water mark of the last line (0 = none yet)
uint8_t  s_heapLines  = 0;
uint8_t  s_lostLines  = 0;
uint32_t s_lastPoll   = 0;
uint32_t s_lastLost   = 0;

} // namespace

void heapActivityBegin(HeapActivityKind k, const char* what) {
    if (k >= HA_COUNT) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    Slot& s = s_slots[k];
    strlcpy(s.what, what ? what : "?", sizeof(s.what));
    s.startMs = now;
    s.active  = true;
    portEXIT_CRITICAL(&s_mux);
}

void heapActivityEnd(HeapActivityKind k) {
    if (k >= HA_COUNT) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    s_slots[k].endMs  = now;
    s_slots[k].active = false;
    portEXIT_CRITICAL(&s_mux);
}

void dataLost(const char* where, uint32_t n) {
    if (!n) return;
    portENTER_CRITICAL(&s_mux);   // also makes the += safe across tasks
    g_queueDrops  += n;
    s_lostPending += n;
    s_lostWhere    = where;
    portEXIT_CRITICAL(&s_mux);
}

// "web=/api/data(2s ago) export=openSenseMap(running)" — the activities
// running now or finished within RECENT_MS, or "none".
static void describeActivities(uint32_t now, char* out, size_t cap) {
    Slot copy[HA_COUNT];
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, s_slots, sizeof(copy));
    portEXIT_CRITICAL(&s_mux);

    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < HA_COUNT && len < cap; i++) {
        const Slot& s = copy[i];
        if (!s.startMs) continue;
        // Signed: another task may have stamped after `now` was read.
        const int32_t ago = (int32_t)(now - s.endMs);
        if (!s.active && ago > (int32_t)RECENT_MS) continue;
        const int32_t run = (int32_t)(now - s.startMs);
        const int n = s.active
            ? snprintf(out + len, cap - len, "%s%s=%s(running %lds)", len ? " " : "",
                       KIND_NAMES[i], s.what, (long)(run > 0 ? run / 1000 : 0))
            : snprintf(out + len, cap - len, "%s%s=%s(%lds ago)", len ? " " : "",
                       KIND_NAMES[i], s.what, (long)(ago > 0 ? ago / 1000 : 0));
        if (n < 0) break;
        len += (size_t)n;
    }
    if (!len) strlcpy(out, "none", cap);
}

void heapWatchTick(uint32_t now) {
    if ((int32_t)(now - s_lastPoll) < (int32_t)POLL_MS) return;
    s_lastPoll = now;

    const uint32_t minFree = ESP.getMinFreeHeap();
    if (minFree < HEAP_WATCH_LOW_BYTES && s_heapLines < HEAP_LINES_MAX &&
        (!s_heapLogged || minFree + HEAP_STEP_BYTES <= s_heapLogged)) {
        s_heapLogged = minFree;
        s_heapLines++;
        char acts[96], at[24];
        describeActivities(now, acts, sizeof(acts));
        eventLogNow(at, sizeof(at));
        Serial.printf("[heap] low: min=%u free=%u largest=%u during %s\n",
                      (unsigned)minFree, (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap(), acts);
        eventLogPrintf("low heap min=%u free=%u largest=%u at=%s up=%lus  %s",
                       (unsigned)minFree, (unsigned)ESP.getFreeHeap(),
                       (unsigned)ESP.getMaxAllocHeap(), at,
                       (unsigned long)(now / 1000), acts);
    }

    if ((int32_t)(now - s_lastLost) < (int32_t)LOST_EVERY_MS && s_lastLost) return;
    uint32_t lost;
    const char* where;
    portENTER_CRITICAL(&s_mux);
    lost  = s_lostPending;
    where = s_lostWhere;
    s_lostPending = 0;
    portEXIT_CRITICAL(&s_mux);
    if (!lost) return;
    s_lastLost = now ? now : 1;
    if (s_lostLines >= LOST_LINES_MAX) return;
    s_lostLines++;
    char at[24];
    eventLogNow(at, sizeof(at));
    eventLogPrintf("data lost n=%lu last=%s total=%lu at=%s up=%lus",
                   (unsigned long)lost, where ? where : "?",
                   (unsigned long)g_queueDrops, at, (unsigned long)(now / 1000));
}
