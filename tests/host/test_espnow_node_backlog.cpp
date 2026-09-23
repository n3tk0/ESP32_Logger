// The ESP-NOW node's undelivered-readings queue (node_espnow/src/Backlog.h).
//
// WHY THIS FILE EXISTS
// --------------------
// The queue lives in RTC memory across deep sleep and holds readings of a
// VARIABLE shape now — one value for a lone light sensor, nine for a full
// config plus battery — packed back to back in a kilobyte. Everything that
// can go wrong with it goes wrong quietly: a frame that drops the live
// reading still validates, an eviction that drops the newest still reports a
// plausible count, an ACK that clears more than the frame carried loses
// history nobody knew was there, and a walk off the end of a half-written
// pool is a crash on a device on a roof. Each of those is provoked here, and
// every frame built is run through the collector's own espnowValidate() and
// read back with its DATA2 cursor — the only reader that matters.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "node_espnow/src/Backlog.h"
#include "src/nodecfg/MetricCatalog.h"
#include "check.h"

using namespace enbl;

// A BME280 + battery sample: four values, the common node.
static uint8_t bme(Data2Value* v, float t) {
    v[0] = Data2Value{nodecfg::M_TEMPERATURE, 0, t};
    v[1] = Data2Value{nodecfg::M_HUMIDITY, 0, 50.0f};
    v[2] = Data2Value{nodecfg::M_PRESSURE, 0, 1013.2f};
    v[3] = Data2Value{nodecfg::M_BATTERY_VOLTAGE, 0, 3.91f};
    return 4;
}

// The widest sample a valid config produces: eight probes + battery.
static uint8_t wide(Data2Value* v, float t) {
    for (uint8_t i = 0; i < 8; i++) v[i] = Data2Value{nodecfg::M_PROBE_TEMP, i, t + i};
    v[8] = Data2Value{nodecfg::M_BATTERY_VOLTAGE, 0, 3.7f};
    return 9;
}

static Pool g_pool;   // 1 KB: static, like the RTC one

/// Build, validate as the collector does, and decode every sample.
struct Decoded {
    int         len;
    Data2Header hdr;
    uint8_t     count;
    Data2Sample s[40];
};
static Decoded build(const Pool& p, uint32_t now, const Data2Value* live, uint8_t liveN,
                     uint8_t& taken) {
    Decoded d{};
    uint8_t buf[ESPNOW_MAX_FRAME];
    d.len = buildFrame(p, buf, sizeof(buf), 7, 1234, EN_FLAG_FIRST_BOOT, now, live, liveN, taken);
    CHECK(d.len > 0);
    if (d.len <= 0) return d;
    uint8_t type = 0;
    CHECK(espnowValidate(buf, d.len, type));
    CHECK_EQ(type, EN_MSG_DATA2);
    Data2Cursor cur;
    espnowData2Open(buf, d.len, d.hdr, cur);
    while (d.count < 40 && espnowData2Next(cur, d.s[d.count])) d.count++;
    CHECK_EQ(d.count, d.hdr.count);
    return d;
}

// ---------------------------------------------------------------------------
static void test_an_empty_queue_sends_just_the_live_reading() {
    clear(g_pool);
    CHECK(valid(g_pool));
    Data2Value v[EN_DATA2_MAX_VALUES];
    const uint8_t n = bme(v, 21.5f);
    uint8_t taken = 99;
    Decoded d = build(g_pool, 1750000000u, v, n, taken);
    CHECK_EQ(taken, 0);
    CHECK_EQ(d.count, 1);
    CHECK_EQ(d.hdr.nodeId, 7);
    CHECK_EQ(d.hdr.seq, 1234);
    CHECK_EQ(d.hdr.flags, EN_FLAG_FIRST_BOOT);
    CHECK_EQ(d.hdr.epoch, 1750000000u);
    CHECK_EQ(d.s[0].dt_s, 0);
    CHECK_EQ(d.s[0].n, 4);
    CHECK(d.s[0].v[0].value == 21.5f);
    CHECK_EQ(d.s[0].v[3].metric, nodecfg::M_BATTERY_VOLTAGE);
    CHECK_EQ(d.len, EN_DATA2_HDR + espnowData2SampleLen(4));
}

// ---------------------------------------------------------------------------
static void test_buffered_readings_go_first_oldest_first_with_their_age() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    const uint32_t t0 = 1750000000u;
    for (int i = 0; i < 3; i++) CHECK(push(g_pool, t0 + 60u * i, v, bme(v, 10.0f + i)));
    CHECK_EQ(g_pool.count, 3);
    CHECK(valid(g_pool));

    uint8_t taken = 0;
    const uint8_t n = bme(v, 99.0f);
    Decoded d = build(g_pool, t0 + 180, v, n, taken);
    CHECK_EQ(taken, 3);
    CHECK_EQ(d.count, 4);
    // Oldest first, each with its age now; the live reading last, dt 0 — the
    // collector's "newest sample is the current value" rule depends on it.
    CHECK_EQ(d.s[0].dt_s, 180);
    CHECK_EQ(d.s[1].dt_s, 120);
    CHECK_EQ(d.s[2].dt_s, 60);
    CHECK_EQ(d.s[3].dt_s, 0);
    CHECK(d.s[0].v[0].value == 10.0f);
    CHECK(d.s[2].v[0].value == 12.0f);
    CHECK(d.s[3].v[0].value == 99.0f);
}

// ---------------------------------------------------------------------------
static void test_no_clock_means_age_zero_not_fifty_five_years() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    push(g_pool, 0, v, bme(v, 1.0f));           // buffered before any clock
    push(g_pool, 1750000000u, v, bme(v, 2.0f)); // clock arrived later
    uint8_t taken = 0;
    const uint8_t n = bme(v, 3.0f);
    Decoded d = build(g_pool, 0, v, n, taken);  // and none now
    CHECK_EQ(d.s[0].dt_s, 0);
    CHECK_EQ(d.s[1].dt_s, 0);
    d = build(g_pool, 1750000000u + 30, v, n, taken);
    CHECK_EQ(d.s[0].dt_s, 0);                   // taken with no clock: unknown
    CHECK_EQ(d.s[1].dt_s, 30);
    // A clock that went backwards, and an age past the field.
    CHECK_EQ(ageOf(100, 200), 0);
    CHECK_EQ(ageOf(1750000000u + 100000u, 1750000000u), 65535);
}

// ---------------------------------------------------------------------------
static void test_a_frame_carries_as_many_whole_samples_as_fit_and_no_more() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    for (int i = 0; i < 20; i++) push(g_pool, 1750000000u + (uint32_t)i, v, bme(v, (float)i));
    uint8_t taken = 0;
    const uint8_t n = bme(v, 50.0f);
    Decoded d = build(g_pool, 1750000100u, v, n, taken);
    // 12 + 8 × 27 = 228 ≤ 250 < 255: seven buffered + live.
    CHECK_EQ(taken, 7);
    CHECK_EQ(d.count, 8);
    CHECK(d.len <= ESPNOW_MAX_FRAME);
    CHECK(d.len + espnowData2SampleLen(4) > ESPNOW_MAX_FRAME);
    CHECK(d.s[0].v[0].value == 0.0f);           // the oldest ones
    CHECK(d.s[6].v[0].value == 6.0f);
    CHECK(d.s[7].v[0].value == 50.0f);          // live, never displaced

    // Widest samples: 12 + 4 × 57 = 240, so three buffered + live.
    clear(g_pool);
    for (int i = 0; i < 10; i++) push(g_pool, 0, v, wide(v, (float)i));
    const uint8_t wn = wide(v, 80.0f);
    d = build(g_pool, 0, v, wn, taken);
    CHECK_EQ(taken, 3);
    CHECK_EQ(d.count, 4);
    CHECK_EQ(d.s[3].n, 9);
    CHECK_EQ(d.s[3].v[7].index, 7);
}

// ---------------------------------------------------------------------------
static void test_mixed_shapes_after_a_config_change_keep_their_own_ids() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    push(g_pool, 0, v, bme(v, 5.0f));
    Data2Value lux[2] = {{nodecfg::M_LUX, 0, 1234.0f}, {nodecfg::M_BATTERY_VOLTAGE, 0, 3.8f}};
    push(g_pool, 0, lux, 2);
    push(g_pool, 0, nullptr, 0);                 // every sensor failed, no divider
    CHECK_EQ(g_pool.count, 3);
    CHECK(valid(g_pool));

    uint8_t taken = 0;
    Decoded d = build(g_pool, 0, lux, 1, taken);
    CHECK_EQ(taken, 3);
    CHECK_EQ(d.s[0].n, 4);
    CHECK_EQ(d.s[1].n, 2);
    CHECK_EQ(d.s[1].v[0].metric, nodecfg::M_LUX);
    CHECK(d.s[1].v[0].value == 1234.0f);
    CHECK_EQ(d.s[2].n, 0);                       // an empty sample is still a sample
    CHECK_EQ(d.s[3].n, 1);

    uint32_t e;
    uint8_t  n;
    const uint8_t* vals;
    CHECK(at(g_pool, 1, e, n, vals));
    CHECK_EQ(n, 2);
    CHECK_EQ(vals[0], nodecfg::M_LUX);
    CHECK(!at(g_pool, 3, e, n, vals));
}

// ---------------------------------------------------------------------------
static void test_full_drops_the_oldest_and_keeps_the_newest() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    // 1024 / 29 = 35 BME280 samples fit; push 50.
    for (int i = 0; i < 50; i++) CHECK(push(g_pool, 1000u + (uint32_t)i, v, bme(v, (float)i)));
    CHECK_EQ(g_pool.count, POOL_BYTES / entryLen(4));
    CHECK(g_pool.used <= POOL_BYTES);
    CHECK(valid(g_pool));
    uint32_t e;
    uint8_t  n;
    const uint8_t* vals;
    CHECK(at(g_pool, 0, e, n, vals));
    CHECK_EQ(e, 1000u + 50u - g_pool.count);     // the oldest ones went
    CHECK(at(g_pool, (uint8_t)(g_pool.count - 1), e, n, vals));
    CHECK_EQ(e, 1049u);                          // the newest is there

    // A wide sample into a pool full of narrow ones evicts as many as needed.
    CHECK(push(g_pool, 2000u, v, wide(v, 0.0f)));
    CHECK(valid(g_pool));
    CHECK(at(g_pool, (uint8_t)(g_pool.count - 1), e, n, vals));
    CHECK_EQ(e, 2000u);
    CHECK_EQ(n, 9);

    // Too many values for any sample is refused, not truncated.
    Data2Value ten[10] = {};
    CHECK(!push(g_pool, 1, ten, 10));
    CHECK(valid(g_pool));
}

// ---------------------------------------------------------------------------
static void test_an_ack_removes_exactly_what_the_frame_carried() {
    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    for (int i = 0; i < 20; i++) push(g_pool, 1750000000u + (uint32_t)i, v, bme(v, (float)i));
    uint8_t taken = 0;
    const uint8_t n = bme(v, 0.0f);
    build(g_pool, 1750000100u, v, n, taken);
    CHECK_EQ(taken, 7);
    CHECK_EQ(dropOldest(g_pool, taken), 7);
    CHECK_EQ(g_pool.count, 13);
    CHECK(valid(g_pool));
    uint32_t e;
    uint8_t  m;
    const uint8_t* vals;
    CHECK(at(g_pool, 0, e, m, vals));
    CHECK_EQ(e, 1750000007u);                    // the next undelivered one

    // Draining over wakes: 13 left → 7 + 6, and the queue is empty.
    build(g_pool, 1750000100u, v, n, taken);
    dropOldest(g_pool, taken);
    build(g_pool, 1750000100u, v, n, taken);
    CHECK_EQ(taken, 6);
    dropOldest(g_pool, taken);
    CHECK_EQ(g_pool.count, 0);
    CHECK_EQ(g_pool.used, 0);
    CHECK(valid(g_pool));
    CHECK_EQ(dropOldest(g_pool, 5), 0);          // nothing to drop is not an error
}

// ---------------------------------------------------------------------------
static void test_garbage_in_rtc_memory_is_detected() {
    // What a cold boot, or a brownout halfway through push(), leaves behind.
    memset(&g_pool, 0xA5, sizeof(g_pool));
    CHECK(!valid(g_pool));

    clear(g_pool);
    Data2Value v[EN_DATA2_MAX_VALUES];
    push(g_pool, 1, v, bme(v, 1.0f));
    push(g_pool, 2, v, bme(v, 2.0f));
    CHECK(valid(g_pool));

    Pool bad = g_pool;
    bad.count = 3;                                // count disagrees with the walk
    CHECK(!valid(bad));
    bad = g_pool;
    bad.used = (uint16_t)(bad.used - 1);          // an entry runs off the end
    CHECK(!valid(bad));
    bad = g_pool;
    bad.bytes[4] = 200;                           // n past EN_DATA2_MAX_VALUES
    CHECK(!valid(bad));
    bad = g_pool;
    bad.used = POOL_BYTES + 1;
    CHECK(!valid(bad));
}

int main() {
    RUN(test_an_empty_queue_sends_just_the_live_reading);
    RUN(test_buffered_readings_go_first_oldest_first_with_their_age);
    RUN(test_no_clock_means_age_zero_not_fifty_five_years);
    RUN(test_a_frame_carries_as_many_whole_samples_as_fit_and_no_more);
    RUN(test_mixed_shapes_after_a_config_change_keep_their_own_ids);
    RUN(test_full_drops_the_oldest_and_keeps_the_newest);
    RUN(test_an_ack_removes_exactly_what_the_frame_carried);
    RUN(test_garbage_in_rtc_memory_is_detected);
    return SUMMARY();
}
