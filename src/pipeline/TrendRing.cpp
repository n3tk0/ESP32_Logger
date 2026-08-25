#include "TrendRing.h"

#if defined(FEATURE_KINDLE_DASHBOARD)

#include <math.h>
#include <string.h>

TrendRing trendRing;

// Readings stamped before this are pre-NTP millis fallback, not wall clock.
// Folding them in would index a bucket from a 1970 hour and scribble over a
// real one. Same threshold ProcessingTask uses to gate alert evaluation.
static constexpr uint32_t MIN_REAL_TS = 1000000000u;

int TrendRing::_find(const char* sensorId, const char* metric) const {
    for (int i = 0; i < MAX_SERIES; i++) {
        if (!_s[i].used) continue;
        if (strcmp(_s[i].sensorId, sensorId) == 0 &&
            strcmp(_s[i].metric,   metric)   == 0) return i;
    }
    return -1;
}

bool TrendRing::track(const char* sensorId, const char* metric) {
    if (!sensorId || !metric || !*sensorId || !*metric) return false;

    taskENTER_CRITICAL(&_mux);
    int idx = _find(sensorId, metric);
    if (idx < 0) {
        for (int i = 0; i < MAX_SERIES; i++) {
            if (_s[i].used) continue;
            strncpy(_s[i].sensorId, sensorId, sizeof(_s[i].sensorId) - 1);
            _s[i].sensorId[sizeof(_s[i].sensorId) - 1] = '\0';
            strncpy(_s[i].metric, metric, sizeof(_s[i].metric) - 1);
            _s[i].metric[sizeof(_s[i].metric) - 1] = '\0';
            _s[i].used     = true;
            _s[i].lastHour = 0;
            memset(_s[i].h, 0, sizeof(_s[i].h));
            idx = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&_mux);

    return idx >= 0;
}

void TrendRing::add(const SensorReading& r) {
    if (!isfinite(r.value))        return;
    if (r.timestamp < MIN_REAL_TS) return;

    const uint32_t hour = r.timestamp / 3600u;
    const int      slot = (int)(hour % HOURS);

    taskENTER_CRITICAL(&_mux);
    const int i = _find(r.sensorId, r.metric);
    if (i >= 0) {
        Series& s = _s[i];

        // A bucket belongs to exactly one absolute hour. When the incoming
        // reading is from a later hour than anything seen, every slot it
        // skipped past is stale by a full day and must be cleared, not just
        // the one being written — otherwise a gap in reporting leaves
        // day-old buckets sitting in the middle of the window pretending to
        // be recent.
        if (hour > s.lastHour) {
            const uint32_t skipped = hour - s.lastHour;
            if (skipped >= (uint32_t)HOURS) {
                memset(s.h, 0, sizeof(s.h));          // whole window expired
            } else {
                for (uint32_t k = 1; k <= skipped; k++) {
                    memset(&s.h[(s.lastHour + k) % HOURS], 0, sizeof(Hour));
                }
            }
            s.lastHour = hour;
        }

        // Older than the window, or a straggler from a past hour whose slot
        // has already been recycled: dropping is correct, because folding it
        // in would attribute it to whatever hour now owns that slot.
        const bool inWindow = (hour <= s.lastHour) &&
                              (s.lastHour - hour < (uint32_t)HOURS);
        if (inWindow) {
            Hour& b = s.h[slot];
            if (b.count == 0) {
                b.min = b.max = b.sum = r.value;
                b.count = 1;
            } else {
                if (r.value < b.min) b.min = r.value;
                if (r.value > b.max) b.max = r.value;
                b.sum += r.value;
                if (b.count < UINT16_MAX) b.count++;
            }
        }
    }
    taskEXIT_CRITICAL(&_mux);
}

bool TrendRing::series(const char* sensorId, const char* metric,
                       uint32_t nowTs, Hour* out) const {
    if (!out) return false;

    const uint32_t nowHour = (nowTs >= MIN_REAL_TS) ? (nowTs / 3600u) : 0;
    bool found = false;

    taskENTER_CRITICAL(&_mux);
    const int i = _find(sensorId, metric);
    if (i >= 0) {
        const Series& s = _s[i];
        // Emit oldest → newest ending at the current hour, so the caller can
        // treat index 0..23 as "23 hours ago .. now" without knowing where
        // the ring's write head happens to be.
        for (int k = 0; k < HOURS; k++) {
            const uint32_t h = nowHour - (uint32_t)(HOURS - 1 - k);
            const bool live = (h <= s.lastHour) &&
                              (s.lastHour - h < (uint32_t)HOURS);
            out[k] = live ? s.h[h % HOURS] : Hour{0, 0, 0, 0};
        }
        found = true;
    }
    taskEXIT_CRITICAL(&_mux);

    return found;
}

#endif  // FEATURE_KINDLE_DASHBOARD
