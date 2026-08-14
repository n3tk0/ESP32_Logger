// Host unit tests for the header-only RingBuffer in src/pipeline/DataPipeline.h.
// Capacity is a runtime argument to begin(); these build the internal-RAM
// path (preferPsram=false), which is also what the ESP32-C3 targets use.
// (single-threaded correctness: ordering, overflow, time filter, lookups).
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

int main() {
    RUN(test_push_and_copy_order);
    RUN(test_overflow_keeps_most_recent);
    RUN(test_copyRecent_fromTs_filter);
    RUN(test_findLast);
    RUN(test_collectMetricSeries_chronological);
    RUN(test_copyRecent_window_is_newest);
    RUN(test_copyRecent_window_with_fromTs);
    RUN(test_scan_limits_bound_backward_walk);
    RUN(test_scan_limits_leave_small_ring_unchanged);
    return SUMMARY();
}
