// Host unit tests for the header-only RingBuffer in src/pipeline/DataPipeline.h.
// Capacity is a runtime argument to begin(); these build the internal-RAM
// path (preferPsram=false), which is also what the ESP32-C3 targets use.
// (single-threaded correctness: ordering, overflow, time filter, lookups).
// test_ringbuffer_compact.cpp builds this same file with RING_COMPACT=1, the
// C3's storage, and adds the key-table tests at the bottom.
#include "src/pipeline/DataPipeline.h"
#include "check.h"

static SensorReading mk(const char* id, const char* metric,
                        float value, uint32_t ts) {
    return SensorReading::make(ts, id, "t", metric, value, "u", QUALITY_GOOD);
}

static void test_push_and_copy_order() {
    RingBuffer rb;
    CHECK(rb.begin(8, /*preferPsram=*/false));
    for (int i = 0; i < 5; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[8];
    size_t n = rb.copyRecent(out, 8);
    CHECK_EQ(n, (size_t)5);
    bool ordered = true;
    for (int i = 0; i < 5; i++)
        if (out[i].value != (float)i || out[i].timestamp != (uint32_t)i) ordered = false;
    CHECK(ordered);
    CHECK_EQ(rb.size(), (size_t)5);
}

static void test_overflow_keeps_most_recent() {
    RingBuffer rb;
    CHECK(rb.begin(4, /*preferPsram=*/false));
    for (int i = 0; i < 10; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[16];
    size_t n = rb.copyRecent(out, 16);
    CHECK_EQ(n, (size_t)4);                 // capped at N
    CHECK_EQ((int)out[0].value, 6);         // oldest retained
    CHECK_EQ((int)out[3].value, 9);         // newest
    CHECK_EQ(rb.size(), (size_t)4);
}

static void test_copyRecent_fromTs_filter() {
    RingBuffer rb;
    CHECK(rb.begin(16, /*preferPsram=*/false));
    for (int i = 0; i < 10; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[16];
    size_t n = rb.copyRecent(out, 16, /*fromTs=*/7);
    CHECK_EQ(n, (size_t)3);                 // ts 7,8,9
    CHECK_EQ((int)out[0].timestamp, 7);
    CHECK_EQ((int)out[2].timestamp, 9);
}

static void test_findLast() {
    RingBuffer rb;
    CHECK(rb.begin(16, /*preferPsram=*/false));
    rb.push(mk("s", "m", 1.0f, 1));
    rb.push(mk("s", "x", 2.0f, 2));   // different metric
    rb.push(mk("s", "m", 3.0f, 3));   // most recent "m"

    SensorReading out;
    CHECK(rb.findLast("s", "m", out));
    CHECK_EQ((int)out.value, 3);
    CHECK(!rb.findLast("s", "absent", out));
}

static void test_collectMetricSeries_chronological() {
    RingBuffer rb;
    CHECK(rb.begin(16, /*preferPsram=*/false));
    rb.push(mk("s", "m", 10.0f, 1));
    rb.push(mk("s", "x", 99.0f, 2));  // unrelated metric ignored
    rb.push(mk("s", "m", 20.0f, 3));
    rb.push(mk("s", "m", 30.0f, 4));

    float out[8];
    size_t n = rb.collectMetricSeries("s", "m", out, 8);
    CHECK_EQ(n, (size_t)3);
    CHECK_EQ((int)out[0], 10);   // oldest -> newest
    CHECK_EQ((int)out[1], 20);
    CHECK_EQ((int)out[2], 30);
}

// Regression: copyRecent must return the NEWEST maxOut entries, not the oldest.
// The old implementation scanned forward from the oldest index and stopped at
// maxOut, which returned the oldest window. That was masked while the ring was
// smaller than every caller's maxOut; it became wrong the moment the ring could
// outgrow it (PSRAM-backed capacity).
static void test_copyRecent_window_is_newest() {
    RingBuffer rb;
    CHECK(rb.begin(64, /*preferPsram=*/false));
    for (int i = 0; i < 64; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[4];
    size_t n = rb.copyRecent(out, 4);
    CHECK_EQ(n, (size_t)4);
    // Newest four are 60..63, in chronological order.
    CHECK(out[0].value == 60.0f);
    CHECK(out[1].value == 61.0f);
    CHECK(out[2].value == 62.0f);
    CHECK(out[3].value == 63.0f);

    // Same after the ring has wrapped: push another 64 so indices 64..127 are
    // live, then the newest three must be 125, 126, 127.
    for (int i = 64; i < 128; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));
    SensorReading out3[3];
    CHECK_EQ(rb.copyRecent(out3, 3), (size_t)3);
    CHECK(out3[0].value == 125.0f);
    CHECK(out3[2].value == 127.0f);

    // A maxOut larger than the ring still yields everything it holds.
    SensorReading big[128];
    CHECK_EQ(rb.copyRecent(big, 128), (size_t)64);
    CHECK(big[0].value == 64.0f);     // oldest live entry
    CHECK(big[63].value == 127.0f);   // newest
}

// The fromTs filter applies WITHIN the newest window, so a cutoff inside it
// trims a prefix and legitimately returns fewer than maxOut.
static void test_copyRecent_window_with_fromTs() {
    RingBuffer rb;
    CHECK(rb.begin(64, /*preferPsram=*/false));
    for (int i = 0; i < 64; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[10];
    size_t n = rb.copyRecent(out, 10, /*fromTs=*/60);
    CHECK_EQ(n, (size_t)4);           // ts 60..63 out of the newest ten
    CHECK(out[0].value == 60.0f);
    CHECK(out[3].value == 63.0f);
}

// findLast / collectMetricSeries walk backward from the head and are bounded by
// RING_SCAN_LIMIT_*, so a metric that stopped reporting longer ago than the
// limit reads as absent. That bound is what stops /api/sensors from walking a
// 58 000-entry PSRAM ring once per missing metric while holding webDataMutex.
static void test_scan_limits_bound_backward_walk() {
    RingBuffer rb;
    CHECK(rb.begin(4096, /*preferPsram=*/false));

    // "old" reports once, then goes quiet behind a wall of other readings.
    rb.push(mk("s", "old", 1.0f, 1));
    for (int i = 0; i < 3000; i++) rb.push(mk("s", "new", (float)i, (uint32_t)(i + 2)));

    SensorReading r;
    // Beyond RING_SCAN_LIMIT_LAST (256) → reported absent, not found.
    CHECK(!rb.findLast("s", "old", r));
    // The recent metric is still found immediately.
    CHECK(rb.findLast("s", "new", r));
    CHECK(r.value == 2999.0f);

    // Same bound applies to the sparkline series: "old" has no samples inside
    // RING_SCAN_LIMIT_SERIES (2048), so it comes back empty rather than
    // costing a full-ring scan.
    float spark[8];
    CHECK_EQ(rb.collectMetricSeries("s", "old", spark, 8), (size_t)0);
    // ...while the live metric fills the request from the newest end.
    CHECK_EQ(rb.collectMetricSeries("s", "new", spark, 8), (size_t)8);
    CHECK(spark[7] == 2999.0f);
}

// A metric within the limit is still found after the ring has wrapped, so the
// bound does not weaken normal operation on the small internal-RAM ring
// (capacity 227 < RING_SCAN_LIMIT_LAST, i.e. behaviour there is unchanged).
static void test_scan_limits_leave_small_ring_unchanged() {
    RingBuffer rb;
    CHECK(rb.begin(227, /*preferPsram=*/false));
    rb.push(mk("s", "rare", 42.0f, 1));
    for (int i = 0; i < 200; i++) rb.push(mk("s", "busy", (float)i, (uint32_t)(i + 2)));

    SensorReading r;
    // 201 entries back — inside both the ring and the 256-entry limit.
    CHECK(rb.findLast("s", "rare", r));
    CHECK(r.value == 42.0f);
}

// copyMatching() sees the same window as copyRecent(out, maxScan, fromTs) and
// keeps what the /api/data filter kept: the time range, sensor and metric.
static void test_copyMatching_filters_window() {
    RingBuffer rb;
    CHECK(rb.begin(32, /*preferPsram=*/false));
    for (int i = 0; i < 20; i++) {
        rb.push(mk("a", "t", (float)i, (uint32_t)(100 + i)));
        rb.push(mk("b", "t", (float)-i, (uint32_t)(100 + i)));
    }
    // Window = newest 10 entries: a/b for ts 115..119.
    size_t n = rb.copyMatching(nullptr, 0, 10, 0, UINT32_MAX, "a", "t", true);
    CHECK_EQ(n, (size_t)5);
    SensorReading out[5];
    CHECK_EQ(rb.copyMatching(out, 5, 10, 0, UINT32_MAX, "a", "t"), (size_t)5);
    CHECK_EQ((int)out[0].timestamp, 115);   // oldest first
    CHECK_EQ((int)out[4].timestamp, 119);
    // Range [116, 118].
    CHECK_EQ(rb.copyMatching(nullptr, 0, 10, 116, 118, "a", "t", true), (size_t)3);
    // No filter: every entry in the window.
    CHECK_EQ(rb.copyMatching(nullptr, 0, 10, 0, UINT32_MAX, nullptr, nullptr, true), (size_t)10);
    CHECK_EQ(rb.copyMatching(nullptr, 0, 10, 0, UINT32_MAX, "c", nullptr, true), (size_t)0);
}

// A buffer smaller than the matches gets the newest of them, oldest first.
static void test_copyMatching_keeps_newest_when_short() {
    RingBuffer rb;
    CHECK(rb.begin(16, /*preferPsram=*/false));
    for (int i = 0; i < 10; i++) rb.push(mk("a", "t", (float)i, (uint32_t)i));
    SensorReading out[3];
    CHECK_EQ(rb.copyMatching(out, 3, 16, 0, UINT32_MAX, "a", "t"), (size_t)3);
    CHECK_EQ((int)out[0].timestamp, 7);
    CHECK_EQ((int)out[2].timestamp, 9);
    // Fewer matches than room: packed at the start.
    SensorReading big[8];
    CHECK_EQ(rb.copyMatching(big, 8, 4, 0, UINT32_MAX, "a", "t"), (size_t)4);
    CHECK_EQ((int)big[0].timestamp, 6);
    CHECK_EQ((int)big[3].timestamp, 9);
}

static void test_latestPerMetric() {
    RingBuffer rb;
    CHECK(rb.begin(16, /*preferPsram=*/false));
    rb.push(mk("a", "t", 1.0f, 1));
    rb.push(mk("a", "h", 2.0f, 2));
    rb.push(mk("b", "t", 3.0f, 3));
    rb.push(mk("a", "t", 4.0f, 4));        // newer a/t
    SensorReading out[8];
    size_t n = rb.latestPerMetric(out, 8, 16);
    CHECK_EQ(n, (size_t)3);
    CHECK_EQ((int)out[0].value, 4);          // newest first
    CHECK_EQ((int)out[1].value, 3);
    CHECK_EQ((int)out[2].value, 2);
    // Scan bound: only the newest 2 entries.
    CHECK_EQ(rb.latestPerMetric(out, 8, 2), (size_t)2);
    // Output bound.
    CHECK_EQ(rb.latestPerMetric(out, 1, 16), (size_t)1);
    CHECK_EQ((int)out[0].value, 4);
}

#if RING_COMPACT
// The whole reading survives the round trip through slot + key table.
static void test_compact_round_trip() {
    RingBuffer rb;
    CHECK(rb.begin(4, false));
    rb.push(SensorReading::make(7, "outside_node_01", "bme680", "temperature",
                                21.5f, "C", QUALITY_ESTIMATED));
    SensorReading r;
    CHECK(rb.findLast("outside_node_01", "temperature", r));
    CHECK_EQ((int)r.timestamp, 7);
    CHECK(strcmp(r.sensorId,   "outside_node_01") == 0);
    CHECK(strcmp(r.sensorType, "bme680") == 0);
    CHECK(strcmp(r.metric,     "temperature") == 0);
    CHECK(strcmp(r.unit,       "C") == 0);
    CHECK(r.value == 21.5f);
    CHECK_EQ((int)r.quality, (int)QUALITY_ESTIMATED);
    // 12 B per reading plus the table, not 72 B per reading.
    CHECK_EQ(rb.bytes(), 4 * (size_t)12 + RING_KEYS * (size_t)60);
}

// Keys whose readings have all been overwritten are reused, so a stream of
// more distinct metrics than RING_KEYS over time never runs out.
static void test_compact_keys_recycle() {
    RingBuffer rb;
    CHECK(rb.begin(8, false));
    char m[16];
    for (int i = 0; i < RING_KEYS * 4; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        rb.push(mk("s", m, (float)i, (uint32_t)i));
    }
    CHECK_EQ(rb.keyDrops(), (uint32_t)0);
    // The newest 8 are intact, each with its own strings.
    SensorReading out[8];
    CHECK_EQ(rb.copyRecent(out, 8), (size_t)8);
    bool ok = true;
    for (int k = 0; k < 8; k++) {
        const int i = RING_KEYS * 4 - 8 + k;
        snprintf(m, sizeof(m), "m%d", i);
        if (strcmp(out[k].metric, m) != 0 || out[k].value != (float)i) ok = false;
    }
    CHECK(ok);
}

// More distinct metrics live in the ring than the table holds: the extra
// readings are dropped and counted, and what was stored stays correct.
static void test_compact_table_full_drops() {
    RingBuffer rb;
    CHECK(rb.begin(RING_KEYS + 8, false));
    char m[16];
    for (int i = 0; i < RING_KEYS + 3; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        rb.push(mk("s", m, (float)i, (uint32_t)i));
    }
    CHECK_EQ(rb.keyDrops(), (uint32_t)3);
    CHECK_EQ(rb.size(), (size_t)RING_KEYS);
    SensorReading r;
    CHECK(rb.findLast("s", "m0", r));
    snprintf(m, sizeof(m), "m%d", RING_KEYS);   // the first one dropped
    CHECK(!rb.findLast("s", m, r));
    // A known key still goes in.
    rb.push(mk("s", "m0", 99.0f, 1000));
    CHECK_EQ(rb.keyDrops(), (uint32_t)3);
    CHECK(rb.findLast("s", "m0", r));
    CHECK(r.value == 99.0f);
}

// A drop on a full ring must not lose the reading it would have evicted:
// that reading keeps its key, so a later push cannot take the entry from it.
static void test_compact_drop_keeps_evicted_key() {
    RingBuffer rb;
    CHECK(rb.begin(RING_KEYS + 1, false));
    char m[16];
    rb.push(mk("s", "m0", 0.0f, 0));          // m0 twice: evicting the first
    for (int i = 0; i < RING_KEYS; i++) {     // leaves its key still in use
        snprintf(m, sizeof(m), "m%d", i);
        rb.push(mk("s", m, (float)i, (uint32_t)i + 1));
    }
    // Full ring, full table, new metric: dropped, the oldest m0 stays.
    rb.push(mk("s", "new", 1.0f, 500));
    CHECK_EQ(rb.keyDrops(), (uint32_t)1);
    SensorReading all[RING_KEYS + 1];
    CHECK_EQ(rb.copyRecent(all, RING_KEYS + 1), (size_t)(RING_KEYS + 1));
    CHECK(strcmp(all[0].metric, "m0") == 0);
    CHECK_EQ((int)all[0].timestamp, 0);
    // Two known metrics evict both m0s; then "new" can take m0's entry.
    rb.push(mk("s", "m5", 5.5f, 501));
    rb.push(mk("s", "m6", 6.5f, 502));
    rb.push(mk("s", "new", 2.0f, 503));
    CHECK_EQ(rb.keyDrops(), (uint32_t)1);
    SensorReading r;
    CHECK(rb.findLast("s", "new", r));
    CHECK(r.value == 2.0f);
    CHECK(!rb.findLast("s", "m0", r));
    CHECK(rb.findLast("s", "m2", r));
    CHECK(strcmp(r.metric, "m2") == 0 && r.value == 2.0f);
}
#endif

int main() {
#if RING_COMPACT
    RUN(test_compact_round_trip);
    RUN(test_compact_keys_recycle);
    RUN(test_compact_table_full_drops);
    RUN(test_compact_drop_keeps_evicted_key);
#endif
    RUN(test_push_and_copy_order);
    RUN(test_overflow_keeps_most_recent);
    RUN(test_copyRecent_fromTs_filter);
    RUN(test_findLast);
    RUN(test_collectMetricSeries_chronological);
    RUN(test_copyRecent_window_is_newest);
    RUN(test_copyRecent_window_with_fromTs);
    RUN(test_scan_limits_bound_backward_walk);
    RUN(test_scan_limits_leave_small_ring_unchanged);
    RUN(test_copyMatching_filters_window);
    RUN(test_copyMatching_keeps_newest_when_short);
    RUN(test_latestPerMetric);
    return SUMMARY();
}
