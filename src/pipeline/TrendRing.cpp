#include "TrendRing.h"

// The feature macro lives in setup.h and nothing in TrendRing.h's include
// chain reaches it, so without this the guard below is always false and
// enabling the feature in setup.h yields an undefined reference to trendRing.
#include "../setup.h"

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
            // An hour just finished. That is the one moment a bucket stops
            // changing, and the only moment worth spending a flash write on.
            _dirty = true;
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

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------
//
// A header, every slot verbatim, and a CRC of the lot.
//
// WHY A CRC AND NOT JUST A MAGIC AND A LENGTH, which is what the ESP-NOW node
// table settles for: this file exists precisely because the power goes off.
// The write that saves it is therefore the write most likely to be interrupted
// halfway, and a torn file whose length happens to be right restores buckets
// full of whatever was on that flash page — which is not a blank chart, it is
// a chart of invented temperatures. There is no other way to notice: nothing
// downstream knows what the reading "should" have been.
//
// The whole record is written or none of it, and TrendStore.cpp writes to a
// temporary file and renames, so a snapshot that fails the CRC means the flash
// itself lied rather than that the save was cut short.

namespace {

/// CRC-32 (IEEE 802.3), computed a nibble at a time from a 16-entry table:
/// 64 bytes of table against the 1 KB a byte-wise table would cost, on a part
/// where the whole snapshot is under two.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    static const uint32_t kTable[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kTable[crc & 0x0Fu];
        crc = (crc >> 4) ^ kTable[crc & 0x0Fu];
    }
    return crc;
}

/// What sits in front of the slots. Packed so the on-flash layout is the same
/// whatever the compiler would rather align things to.
struct __attribute__((packed)) SnapHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t slots;        ///< MAX_SERIES of the build that wrote it
    uint16_t hours;        ///< HOURS of the build that wrote it
    uint16_t seriesBytes;  ///< sizeof(Series), so a layout change is refused
    uint32_t crc;          ///< of everything after this field
};

}  // namespace

size_t TrendRing::snapshotBytes() {
    // Inside the member function because Series is private: this is the one
    // place that can see both it and the bound it has to stay under.
    static_assert(sizeof(SnapHeader) + sizeof(Series) * (size_t)MAX_SERIES
                      <= SNAP_MAX_BYTES,
                  "the snapshot outgrew SNAP_MAX_BYTES — raise it, and check "
                  "the stack of whoever calls trendStoreSave()");
    return sizeof(SnapHeader) + sizeof(Series) * (size_t)MAX_SERIES;
}

size_t TrendRing::snapshot(uint8_t* buf, size_t cap) const {
    const size_t need = snapshotBytes();
    if (buf == nullptr || cap < need) return 0;

    SnapHeader h{};
    h.magic       = SNAP_MAGIC;
    h.version     = SNAP_VERSION;
    h.slots       = (uint16_t)MAX_SERIES;
    h.hours       = (uint16_t)HOURS;
    h.seriesBytes = (uint16_t)sizeof(Series);

    uint8_t* payload = buf + sizeof(SnapHeader);
    taskENTER_CRITICAL(&_mux);
    memcpy(payload, _s, sizeof(Series) * (size_t)MAX_SERIES);
    taskEXIT_CRITICAL(&_mux);

    h.crc = crc32(payload, sizeof(Series) * (size_t)MAX_SERIES);
    memcpy(buf, &h, sizeof(h));
    return need;
}

bool TrendRing::restore(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len != snapshotBytes()) return false;

    SnapHeader h{};
    memcpy(&h, buf, sizeof(h));
    if (h.magic       != SNAP_MAGIC)             return false;
    if (h.version     != SNAP_VERSION)           return false;
    if (h.slots       != (uint16_t)MAX_SERIES)   return false;
    if (h.hours       != (uint16_t)HOURS)        return false;
    if (h.seriesBytes != (uint16_t)sizeof(Series)) return false;

    const uint8_t* payload = buf + sizeof(SnapHeader);
    const size_t   bytes   = sizeof(Series) * (size_t)MAX_SERIES;
    if (crc32(payload, bytes) != h.crc) return false;

    // Copied into a staging array and checked before anything reaches the live
    // ring: a series whose id or metric is not NUL-terminated would be read
    // past by _find()'s strcmp on every reading from then on.
    Series staged[MAX_SERIES];
    memcpy(staged, payload, bytes);
    for (int i = 0; i < MAX_SERIES; i++) {
        staged[i].sensorId[sizeof(staged[i].sensorId) - 1] = '\0';
        staged[i].metric[sizeof(staged[i].metric)   - 1] = '\0';
        // A used slot with no name is not a series, it is a corrupted one.
        if (staged[i].used && staged[i].sensorId[0] == '\0') return false;
    }

    // MERGED BY NAME INTO THE SERIES THIS BUILD TRACKS, not copied wholesale
    // over them. Which series exist is a decision the firmware makes at boot
    // from the current configuration; the file only supplies their history.
    //
    // Overwriting the array instead would let a snapshot decide the tracked
    // set, and there are only MAX_SERIES slots: rename the outdoor sensor, or
    // build with a different indoor id, and a restored file would fill every
    // slot with series nothing feeds any more — leaving track() no room for
    // the ones that matter and no way to say so. Those entries are simply not
    // claimed here, and the data they held goes with them, which is the right
    // answer for a series that no longer exists.
    taskENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_SERIES; i++) {
        if (!staged[i].used) continue;
        const int j = _find(staged[i].sensorId, staged[i].metric);
        if (j < 0) continue;
        _s[j].lastHour = staged[i].lastHour;
        memcpy(_s[j].h, staged[i].h, sizeof(_s[j].h));
    }
    taskEXIT_CRITICAL(&_mux);
    return true;
}

#endif  // FEATURE_KINDLE_DASHBOARD
