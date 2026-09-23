// ============================================================================
// node_espnow/src/Backlog.h
//
// The readings the node could not deliver, kept in RTC memory across deep
// sleep, and the DATA2 frame built from them.
//
// WHY THIS IS A BYTE POOL AND NOT AN ARRAY OF SAMPLES
// ---------------------------------------------------
// The node used to have exactly one sensor, so a buffered reading was a fixed
// 12-byte EnvSample and the queue was EnvSample[14]. A configured node
// publishes anything from one value (a lone light sensor, battery absent) to
// nine (eight metrics plus battery_voltage), so a fixed slot sized for the
// worst case would hold a third as many readings for the common BME280 node
// as it could. Entries are therefore variable length, packed back to back:
//
//     epoch u32 | n u8 | n × Data2Value (metric u8, index u8, value f32)
//
// which is DATA2's own per-value layout, so building a frame is a copy. Each
// entry carries its own metric ids, so a config change between a reading
// being buffered and being sent cannot relabel it.
//
// WHAT IS KEPT FROM THE OLD QUEUE
// -------------------------------
// * Full means drop the OLDEST: losing the start of an outage beats losing the
//   end of it — the recent readings say what the weather is doing now.
// * dt_s is computed when the frame is built, not stored: it is an age, and
//   ages change while a sample waits. No clock on either side → 0, and the
//   collector stamps on arrival, which is the honest answer.
// * The live reading is always the LAST sample in the frame, dt_s 0, and is
//   never displaced by history: the collector treats the newest sample as the
//   current value.
//
// WHAT CHANGED
// ------------
// The pool can hold more than one frame carries (a BME280 node: 35 buffered
// readings, 7 per frame beside the live one), so a long outage drains over
// several wakes, oldest first, instead of being clipped to one frame's worth.
// Only what the ACK confirmed is removed (`dropOldest(taken)`).
//
// Pure C++ (no Arduino), so tests/host/test_espnow_node_backlog.cpp runs it.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "src/espnow/EspNowProto.h"

namespace enbl {

/// Bytes of RTC memory the queue may use. The XIAO C3 has 8 KB of RTC memory
/// and nothing else in this firmware wants more than a few dozen bytes of it;
/// 1 KB holds 35 BME280+battery readings (29 bytes each) or 17 of the widest
/// possible sample (59 bytes).
#ifndef NODE_BACKLOG_BYTES
#  define NODE_BACKLOG_BYTES 1024
#endif
static const uint16_t POOL_BYTES = NODE_BACKLOG_BYTES;
static_assert(NODE_BACKLOG_BYTES >= 5 + 6 * EN_DATA2_MAX_VALUES,
              "the backlog must hold at least one widest sample");
static_assert(NODE_BACKLOG_BYTES <= 0xFFFF, "used is a u16");

/// Bytes one entry of `n` values occupies in the pool.
static inline uint16_t entryLen(uint8_t n) { return (uint16_t)(5 + 6u * n); }

struct Pool {
    uint16_t used;              ///< bytes in use, entries packed from 0
    uint8_t  count;             ///< entries
    uint8_t  bytes[POOL_BYTES];
};

static inline void clear(Pool& p) {
    p.used  = 0;
    p.count = 0;
}

/// Walk the pool and check it is what push() would have produced. RTC memory
/// is not initialised on a cold boot and main.cpp only trusts it after its
/// magic check, but a half-written pool (a brownout mid-push) must not be
/// walked off the end of on the next wake.
static inline bool valid(const Pool& p) {
    if (p.used > POOL_BYTES) return false;
    uint16_t pos = 0;
    uint8_t  k   = 0;
    while (pos < p.used) {
        if ((uint32_t)pos + 5 > p.used) return false;
        const uint8_t n = p.bytes[pos + 4];
        if (n > EN_DATA2_MAX_VALUES) return false;
        pos = (uint16_t)(pos + entryLen(n));
        if (pos > p.used) return false;
        if (++k == 0) return false;          // more than 255 entries: corrupt
    }
    return k == p.count;
}

/// Remove the `k` oldest entries (fewer if there are fewer). Returns how many
/// went.
static inline uint8_t dropOldest(Pool& p, uint8_t k) {
    uint16_t pos = 0;
    uint8_t  gone = 0;
    while (gone < k && gone < p.count && pos < p.used) {
        pos = (uint16_t)(pos + entryLen(p.bytes[pos + 4]));
        gone++;
    }
    if (pos > p.used) pos = p.used;
    memmove(p.bytes, p.bytes + pos, (size_t)(p.used - pos));
    p.used  = (uint16_t)(p.used - pos);
    p.count = (uint8_t)(p.count - gone);
    return gone;
}

/// Buffer one reading taken at `epoch` (0 = no clock). Drops the oldest
/// entries until it fits. False only for a sample no pool could hold
/// (n > EN_DATA2_MAX_VALUES) — an empty sample (every sensor failed and no
/// battery) is kept, because the frame it becomes still proves the node alive.
static inline bool push(Pool& p, uint32_t epoch, const Data2Value* v, uint8_t n) {
    if (n > EN_DATA2_MAX_VALUES || (n && !v)) return false;
    const uint16_t need = entryLen(n);
    while (p.count && ((uint32_t)p.used + need > POOL_BYTES || p.count == 255))
        dropOldest(p, 1);
    if ((uint32_t)p.used + need > POOL_BYTES) return false;   // unreachable, see assert
    uint8_t* e = p.bytes + p.used;
    memcpy(e, &epoch, 4);
    e[4] = n;
    if (n) memcpy(e + 5, v, (size_t)n * sizeof(Data2Value));
    p.used  = (uint16_t)(p.used + need);
    p.count++;
    return true;
}

/// Entry `i` (0 = oldest): its epoch, value count and a pointer to the packed
/// values. False past the end.
static inline bool at(const Pool& p, uint8_t i, uint32_t& epoch, uint8_t& n,
                      const uint8_t*& values) {
    uint16_t pos = 0;
    for (uint8_t k = 0; k < p.count && pos < p.used; k++) {
        const uint8_t m = p.bytes[pos + 4];
        if (k == i) {
            memcpy(&epoch, p.bytes + pos, 4);
            n      = m;
            values = p.bytes + pos + 5;
            return true;
        }
        pos = (uint16_t)(pos + entryLen(m));
    }
    return false;
}

/// Age of a reading taken at `then`, as DATA2's dt_s. 0 when either side has
/// no clock (see usableEpoch() in main.cpp) or the clock went backwards.
static inline uint16_t ageOf(uint32_t now, uint32_t then) {
    if (!now || !then || now <= then) return 0;
    const uint32_t age = now - then;
    return age > 65535u ? (uint16_t)65535u : (uint16_t)age;
}

/// Build a DATA2 frame into `buf`: the oldest buffered readings that fit, in
/// order, then the live one last with dt_s 0. `taken` says how many buffered
/// entries went in — the caller drops exactly that many once the collector
/// has ACKed the frame, and nothing otherwise.
///
/// Returns the frame length, or -1 when even the live sample alone cannot be
/// framed (never, for n <= EN_DATA2_MAX_VALUES — asserted in EspNowProto.h).
static inline int buildFrame(const Pool& p, uint8_t* buf, size_t cap, uint8_t nodeId,
                             uint16_t seq, uint8_t flags, uint32_t now,
                             const Data2Value* live, uint8_t liveN, uint8_t& taken) {
    taken = 0;
    int len = espnowData2Begin(buf, cap, nodeId, seq, flags, now);
    if (len < 0 || liveN > EN_DATA2_MAX_VALUES) return -1;
    const int limit = cap < (size_t)ESPNOW_MAX_FRAME ? (int)cap : ESPNOW_MAX_FRAME;
    const int liveLen = espnowData2SampleLen(liveN);
    if (len + liveLen > limit) return -1;

    Data2Value tmp[EN_DATA2_MAX_VALUES];
    uint16_t pos = 0;
    for (uint8_t k = 0; k < p.count && pos < p.used; k++) {
        uint32_t epoch;
        memcpy(&epoch, p.bytes + pos, 4);
        const uint8_t n = p.bytes[pos + 4];
        if (n > EN_DATA2_MAX_VALUES) break;                    // corrupt: stop here
        if (len + espnowData2SampleLen(n) + liveLen > limit) break;
        if (n) memcpy(tmp, p.bytes + pos + 5, (size_t)n * sizeof(Data2Value));
        if (!espnowData2Append(buf, cap, len, ageOf(now, epoch), tmp, n)) break;
        taken++;
        pos = (uint16_t)(pos + entryLen(n));
    }
    if (!espnowData2Append(buf, cap, len, 0, live, liveN)) return -1;
    return len;
}

}  // namespace enbl
