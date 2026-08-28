// Host unit tests for src/espnow/NodeTable.h
//
// The three decisions the collector makes about an arriving frame, tested
// away from the radio that would otherwise be needed to reach them:
//
//   • which metrics a sample actually contains — an absent field must produce
//     NO metric, because a 0 %RH from a sensor that cannot measure humidity
//     is indistinguishable downstream from a real reading;
//   • whether a sequence number is one to accept — including across the
//     16-bit wrap, which a node reaches after about six weeks;
//   • whether a node has gone quiet — including across the millis() wrap,
//     which the collector reaches after about seven.
//
// Both wraps are the kind of thing that works for weeks and then fails once,
// silently, on a device nobody is watching.
#include <stdint.h>
#include <string.h>

#include "src/espnow/NodeTable.h"
#include "check.h"

static const uint32_t DAY  = 86400u;
static const uint32_t BASE = 1750000000u / DAY * DAY;

static const uint8_t MAC_A[6] = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x03};
static const uint8_t MAC_B[6] = {0x24, 0x6F, 0x28, 0x0A, 0x0B, 0x0C};

// ---------------------------------------------------------------------------
// Expanding a sample
// ---------------------------------------------------------------------------
static void test_expand_full_sample() {
    EnvSample s;
    enClearSample(s);
    s.t_c100   = enPackTemp(21.5f);
    s.rh_x100  = enPackRh(48.25f);
    s.press_pa = enPackPress(101325.0f);
    s.vbat_mv  = enPackMv(3874.0f);

    EspNowMetric m[EN_MAX_SAMPLE_METRICS];
    const int n = espnowExpandSample(s, m, EN_MAX_SAMPLE_METRICS);
    CHECK_EQ(n, 4);

    CHECK_STREQ(m[0].metric, "temperature");
    CHECK_STREQ(m[0].unit,   "C");
    CHECK(m[0].value > 21.49f && m[0].value < 21.51f);

    CHECK_STREQ(m[1].metric, "humidity");
    CHECK_STREQ(m[1].unit,   "%");

    // Pascals on the wire, hectopascals in the pipeline — the same unit every
    // wired sensor in this firmware reports.
    CHECK_STREQ(m[2].metric, "pressure");
    CHECK_STREQ(m[2].unit,   "hPa");
    CHECK(m[2].value > 1013.2f && m[2].value < 1013.3f);

    // Millivolts on the wire, volts in the pipeline.
    CHECK_STREQ(m[3].metric, "battery_voltage");
    CHECK_STREQ(m[3].unit,   "V");
    CHECK(m[3].value > 3.873f && m[3].value < 3.875f);
}

// The case this whole sentinel scheme exists for: a BMP280 has no humidity
// sensor, and must produce three metrics, not four with a zero in it.
static void test_absent_fields_produce_no_metric() {
    EnvSample s;
    enClearSample(s);
    s.t_c100   = enPackTemp(-4.0f);
    s.press_pa = enPackPress(99800.0f);

    EspNowMetric m[EN_MAX_SAMPLE_METRICS];
    const int n = espnowExpandSample(s, m, EN_MAX_SAMPLE_METRICS);
    CHECK_EQ(n, 2);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(m[i].metric, "humidity") != 0);
        CHECK(strcmp(m[i].metric, "battery_voltage") != 0);
    }

    // Nothing measured at all is zero metrics, not a row of zeroes.
    EnvSample empty;
    enClearSample(empty);
    CHECK_EQ(espnowExpandSample(empty, m, EN_MAX_SAMPLE_METRICS), 0);
}

static void test_expand_respects_the_cap() {
    EnvSample s;
    enClearSample(s);
    s.t_c100   = enPackTemp(20.0f);
    s.rh_x100  = enPackRh(50.0f);
    s.press_pa = enPackPress(101000.0f);
    s.vbat_mv  = enPackMv(3900.0f);

    EspNowMetric m[EN_MAX_SAMPLE_METRICS];
    CHECK_EQ(espnowExpandSample(s, m, 2), 2);
    CHECK_EQ(espnowExpandSample(s, m, 0), 0);
    CHECK_EQ(espnowExpandSample(s, nullptr, 4), 0);
}

// ---------------------------------------------------------------------------
// Sequence numbers
// ---------------------------------------------------------------------------
static void test_seq_basic_progression() {
    CHECK_EQ(espnowSeqCheck(false, 0, 0, 0),     EN_SEQ_NEW);      // first ever
    CHECK_EQ(espnowSeqCheck(true, 10, 11, 0),    EN_SEQ_NEW);
    CHECK_EQ(espnowSeqCheck(true, 10, 10, 0),    EN_SEQ_DUPLICATE);
    CHECK_EQ(espnowSeqCheck(true, 10, 9,  0),    EN_SEQ_STALE);

    // A gap — frames lost to the air — is still forward progress.
    CHECK_EQ(espnowSeqCheck(true, 10, 400, 0),   EN_SEQ_NEW);
}

// A 16-bit counter at one frame a minute wraps in about six weeks. Comparing
// numerically would make 0 look older than 65535 and silence the node until
// the counter climbed back — six weeks of nothing, once, with no error.
static void test_seq_survives_the_wrap() {
    CHECK_EQ(espnowSeqCheck(true, 65535, 0, 0), EN_SEQ_NEW);
    CHECK_EQ(espnowSeqCheck(true, 65530, 4, 0), EN_SEQ_NEW);
    CHECK_EQ(espnowSeqCheck(true, 0, 65535, 0), EN_SEQ_STALE);

    // The property behind those cases: exactly half the space ahead of any
    // point is "newer", and it holds from every starting point.
    for (uint32_t last = 0; last < 65536u; last += 977) {
        for (uint32_t d = 1; d < 0x8000u; d += 1013)
            CHECK_EQ(espnowSeqCheck(true, (uint16_t)last,
                                    (uint16_t)(last + d), 0), EN_SEQ_NEW);
        for (uint32_t d = 0x8000u; d < 0x10000u; d += 1013)
            CHECK_EQ(espnowSeqCheck(true, (uint16_t)last,
                                    (uint16_t)(last + d), 0), EN_SEQ_STALE);
    }
}

// A node that resets counts from zero again, so the reset flag has to be
// honoured or the node stays ignored until its counter catches up.
static void test_seq_reset_is_honoured() {
    CHECK_EQ(espnowSeqCheck(true, 5000, 0, EN_FLAG_FIRST_BOOT), EN_SEQ_RESET);
    CHECK_EQ(espnowSeqCheck(true, 5000, 1, EN_FLAG_FIRST_BOOT), EN_SEQ_RESET);

    // But the duplicate test runs first, and that ordering is the only thing
    // limiting replay of a captured first-boot frame: it can be accepted
    // once, and then not again until the real node moves the counter on.
    CHECK_EQ(espnowSeqCheck(true, 0, 0, EN_FLAG_FIRST_BOOT), EN_SEQ_DUPLICATE);

    // Forward progress is not a reset even when the flag is set.
    CHECK_EQ(espnowSeqCheck(true, 10, 11, EN_FLAG_FIRST_BOOT), EN_SEQ_NEW);
}

// ---------------------------------------------------------------------------
// Silence
// ---------------------------------------------------------------------------
static void test_offline_detection() {
    EspNowNode n{};
    n.used = true; n.nodeId = 1; n.intervalS = 60;

    // Provisioned but never heard from is offline, not merely absent.
    CHECK(espnowNodeOffline(n, 1000));

    n.everSeen = true;
    n.lastSeenMs = 1000;

    CHECK(!espnowNodeOffline(n, 1000));
    CHECK(!espnowNodeOffline(n, 1000 + 60000));            // one missed
    CHECK(!espnowNodeOffline(n, 1000 + 180000));           // exactly three
    CHECK(espnowNodeOffline(n,  1000 + 180001));           // past three

    // An unset interval falls back rather than making the node immortal.
    n.intervalS = 0;
    CHECK(espnowNodeOffline(n, 1000 + 1000u * ESPNOW_DEFAULT_INTERVAL_S *
                                     ESPNOW_OFFLINE_INTERVALS + 1));
}

// millis() wraps at about 49 days. A collector that has been up longer must
// not declare every node offline the moment it does.
static void test_offline_survives_the_millis_wrap() {
    EspNowNode n{};
    n.used = true; n.everSeen = true; n.nodeId = 1; n.intervalS = 60;

    n.lastSeenMs = 0xFFFFF000u;                 // shortly before the wrap
    CHECK(!espnowNodeOffline(n, 0xFFFFF000u + 1000));
    CHECK(!espnowNodeOffline(n, 0x00000FFFu));  // 8,191 ms later, across zero
    CHECK(espnowNodeOffline(n,  0x0002BF21u));  // ~180 s later, across zero
}

// ---------------------------------------------------------------------------
// Derived battery metrics
// ---------------------------------------------------------------------------
static void test_battery_metrics_emitted_only_when_meaningful() {
    EspNowMetric m[EN_MAX_NODE_METRICS];
    EspNowNode n{};
    n.used = true; n.nodeId = 1;

    // Never reported a voltage: nothing to say.
    CHECK_EQ(espnowBatteryMetrics(n, m, EN_MAX_NODE_METRICS), 0);
    CHECK(!espnowNodeBatteryWarn(n));

    // A voltage but no history: a percentage, and no remaining-life figure,
    // because there is nothing to extrapolate from.
    n.lastMv = 3900;
    CHECK_EQ(espnowBatteryMetrics(n, m, EN_MAX_NODE_METRICS), 1);
    CHECK_STREQ(m[0].metric, "battery_percent");
    CHECK_STREQ(m[0].unit,   "%");

    // With a fortnight of falling minima, both.
    for (int d = 0; d < 14; d++)
        batteryHistoryAdd(n.batt, BASE + (uint32_t)d * DAY, (uint16_t)(3900 - 2 * d));
    n.lastMv = 3874;
    CHECK_EQ(espnowBatteryMetrics(n, m, EN_MAX_NODE_METRICS), 2);
    CHECK_STREQ(m[1].metric, "battery_days");
    CHECK_STREQ(m[1].unit,   "d");
    CHECK(m[1].value > 200.0f);
}

static void test_battery_warning_reaches_the_table() {
    EspNowNodeTable t;
    EspNowNode* a = t.add(MAC_A, 1, nullptr, 60);
    EspNowNode* b = t.add(MAC_B, 2, nullptr, 60);
    CHECK(a && b);

    a->lastMv = 4100;
    b->lastMv = 4100;
    CHECK(!t.anyBatteryWarn());

    b->lastMv = 3560;                 // under 10 %
    CHECK(t.anyBatteryWarn());
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------
static void test_table_add_and_lookup() {
    EspNowNodeTable t;
    CHECK_EQ(t.count(), 0);

    EspNowNode* a = t.add(MAC_A, 1, nullptr, 60);
    CHECK(a != nullptr);
    CHECK_EQ(t.count(), 1);
    CHECK_STREQ(a->id, "espnow-01");     // default label
    CHECK_EQ((int)a->intervalS, 60);

    CHECK(t.byId(1) == a);
    CHECK(t.byMac(MAC_A) == a);
    CHECK(t.byId(2) == nullptr);
    CHECK(t.byMac(MAC_B) == nullptr);
    CHECK(t.byId(0) == nullptr);         // 0 is "unprovisioned", never a node

    // Re-adding the same node updates it instead of consuming a slot.
    EspNowNode* again = t.add(MAC_B, 1, "outdoor", 300);
    CHECK(again == a);
    CHECK_EQ(t.count(), 1);
    CHECK_STREQ(a->id, "outdoor");
    CHECK_EQ((int)a->intervalS, 300);
    CHECK(t.byMac(MAC_B) == a);          // and follows the new MAC
}

static void test_table_rejects_and_fills() {
    EspNowNodeTable t;
    CHECK(t.add(MAC_A, 0,   nullptr, 60) == nullptr);   // reserved
    CHECK(t.add(MAC_A, 255, nullptr, 60) == nullptr);   // reserved
    CHECK(t.add(nullptr, 1, nullptr, 60) == nullptr);

    for (int i = 1; i <= EspNowNodeTable::CAP; i++) {
        uint8_t mac[6] = {0x02, 0, 0, 0, 0, (uint8_t)i};
        CHECK(t.add(mac, (uint8_t)i, nullptr, 60) != nullptr);
    }
    CHECK_EQ(t.count(), EspNowNodeTable::CAP);

    uint8_t extra[6] = {0x02, 0, 0, 0, 0, 0xEE};
    CHECK(t.add(extra, 200, nullptr, 60) == nullptr);   // full

    // A slot is released only by an explicit remove, never by silence.
    CHECK(t.remove(3));
    CHECK(!t.remove(3));
    CHECK_EQ(t.count(), EspNowNodeTable::CAP - 1);
    CHECK(t.add(extra, 200, nullptr, 60) != nullptr);
}

static void test_table_label_is_clamped() {
    EspNowNodeTable t;
    // Longer than SensorReading::sensorId can hold: it must be cut with a
    // terminator, not run past the field.
    EspNowNode* n = t.add(MAC_A, 1, "a-very-long-node-label-indeed", 60);
    CHECK(n != nullptr);
    CHECK_EQ((int)strlen(n->id), 16);
    CHECK_STREQ(n->id, "a-very-long-node");
}

static void test_offline_count() {
    EspNowNodeTable t;
    EspNowNode* a = t.add(MAC_A, 1, nullptr, 60);
    EspNowNode* b = t.add(MAC_B, 2, nullptr, 60);
    CHECK_EQ(t.offlineCount(500000), 2);           // neither has ever reported

    a->everSeen = true; a->lastSeenMs = 500000;
    b->everSeen = true; b->lastSeenMs = 100000;    // 400 s ago, interval 60
    CHECK_EQ(t.offlineCount(500000), 1);
}

static void test_default_node_id() {
    char buf[17];
    espnowDefaultNodeId(1, buf, sizeof(buf));
    CHECK_STREQ(buf, "espnow-01");
    espnowDefaultNodeId(42, buf, sizeof(buf));
    CHECK_STREQ(buf, "espnow-42");

    // A buffer too small must not be written past.
    char small[4] = {'x', 'x', 'x', 'x'};
    espnowDefaultNodeId(7, small, sizeof(small));
    CHECK_EQ((int)small[0], 0);
}

// ---------------------------------------------------------------------------
// Slot indices
// ---------------------------------------------------------------------------
// indexOf() is what lets state be kept ALONGSIDE the table — the clock-skew
// array in EspNowIngest.cpp — rather than inside EspNowNode, where a new field
// would change sizeof(), make loadNodes() discard the saved file, and cost
// every deployed node a re-pair.
//
// It therefore has to agree with byId() about which slot a node occupies, and
// has to stop answering the moment a node is removed. A stale index would have
// the page showing one node's clock drift under another node's name.
static void test_table_index_of() {
    EspNowNodeTable t;

    CHECK_EQ(t.indexOf(1), -1);          // nothing provisioned
    CHECK_EQ(t.indexOf(0), -1);          // 0 is never a node

    EspNowNode* a = t.add(MAC_A, 1, nullptr, 60);
    EspNowNode* b = t.add(MAC_B, 2, nullptr, 60);
    CHECK(a != nullptr && b != nullptr);

    const int ia = t.indexOf(1);
    const int ib = t.indexOf(2);
    CHECK(ia >= 0 && ib >= 0);
    CHECK(ia != ib);
    CHECK(&t.at(ia) == a);
    CHECK(&t.at(ib) == b);

    CHECK_EQ(t.indexOf(3), -1);          // provisioned neighbours are not a match

    // A removed node stops answering, and its neighbour does not move.
    CHECK(t.remove(1));
    CHECK_EQ(t.indexOf(1), -1);
    CHECK_EQ(t.indexOf(2), ib);
}

// ---------------------------------------------------------------------------
// Clock skew
// ---------------------------------------------------------------------------
static void test_skew_needs_two_clocks() {
    int32_t s = 12345;   // must be left alone on refusal

    CHECK(!espnowClockSkew(0, BASE, s));
    CHECK_EQ((long)s, 12345L);

    CHECK(!espnowClockSkew(BASE, 0, s));
    CHECK_EQ((long)s, 12345L);

    CHECK(!espnowClockSkew(0, 0, s));
    CHECK_EQ((long)s, 12345L);

    // 999999999 is one second below the plausibility floor — an unset clock,
    // not a device that genuinely believes it is September 2001.
    CHECK(!espnowClockSkew(999999999u, BASE, s));
    CHECK(!espnowClockSkew(BASE, 999999999u, s));
}

static void test_skew_sign_and_magnitude() {
    int32_t s = 0;

    // In step.
    CHECK(espnowClockSkew(BASE, BASE, s));
    CHECK_EQ((long)s, 0L);

    // The node is behind us — the direction an RC-timed sleep drifts. Positive.
    CHECK(espnowClockSkew(BASE, BASE - 90u, s));
    CHECK_EQ((long)s, 90L);

    // The node is ahead. Negative — and this is the case the obvious uint32
    // subtraction gets wrong, coming back as 4,294,967,286 instead of -10.
    CHECK(espnowClockSkew(BASE, BASE + 10u, s));
    CHECK_EQ((long)s, -10L);
}

static void test_skew_saturates_without_wrapping() {
    int32_t s = 0;

    // A node that thinks it is 2001 while the collector is in 2025: nearly
    // eight hundred million seconds, well inside the range, so it reports
    // exactly rather than saturating.
    CHECK(espnowClockSkew(1750000000u, 1000000000u, s));
    CHECK_EQ((long)s, 750000000L);

    // Beyond the range, in both directions. The point is not the number, it is
    // that a vast skew must not come back as a small one.
    CHECK(espnowClockSkew(4294967295u, 1000000000u, s));
    CHECK_EQ((long)s, 2147483647L);

    CHECK(espnowClockSkew(1000000000u, 4294967295u, s));
    CHECK_EQ((long)s, -2147483647L);

    // And never INT32_MIN, because every caller takes the magnitude with -skew
    // and negating INT32_MIN is undefined behaviour.
    CHECK(s != (-2147483647L - 1L));
    CHECK_EQ((long)(-s), 2147483647L);
}

int main() {
    RUN(test_expand_full_sample);
    RUN(test_absent_fields_produce_no_metric);
    RUN(test_expand_respects_the_cap);
    RUN(test_seq_basic_progression);
    RUN(test_seq_survives_the_wrap);
    RUN(test_seq_reset_is_honoured);
    RUN(test_offline_detection);
    RUN(test_offline_survives_the_millis_wrap);
    RUN(test_battery_metrics_emitted_only_when_meaningful);
    RUN(test_battery_warning_reaches_the_table);
    RUN(test_table_add_and_lookup);
    RUN(test_table_rejects_and_fills);
    RUN(test_table_label_is_clamped);
    RUN(test_offline_count);
    RUN(test_default_node_id);
    RUN(test_table_index_of);
    RUN(test_skew_needs_two_clocks);
    RUN(test_skew_sign_and_magnitude);
    RUN(test_skew_saturates_without_wrapping);
    return SUMMARY();
}
