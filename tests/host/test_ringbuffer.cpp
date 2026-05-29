// Host unit tests for the header-only RingBuffer<N> in src/pipeline/DataPipeline.h
// (single-threaded correctness: ordering, overflow, time filter, lookups).
#include "src/pipeline/DataPipeline.h"
#include "check.h"

static SensorReading mk(const char* id, const char* metric,
                        float value, uint32_t ts) {
    return SensorReading::make(ts, id, "t", metric, value, "u", QUALITY_GOOD);
}

static void test_push_and_copy_order() {
    RingBuffer<8> rb;
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
    RingBuffer<4> rb;
    for (int i = 0; i < 10; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[16];
    size_t n = rb.copyRecent(out, 16);
    CHECK_EQ(n, (size_t)4);                 // capped at N
    CHECK_EQ((int)out[0].value, 6);         // oldest retained
    CHECK_EQ((int)out[3].value, 9);         // newest
    CHECK_EQ(rb.size(), (size_t)4);
}

static void test_copyRecent_fromTs_filter() {
    RingBuffer<16> rb;
    for (int i = 0; i < 10; i++) rb.push(mk("s", "m", (float)i, (uint32_t)i));

    SensorReading out[16];
    size_t n = rb.copyRecent(out, 16, /*fromTs=*/7);
    CHECK_EQ(n, (size_t)3);                 // ts 7,8,9
    CHECK_EQ((int)out[0].timestamp, 7);
    CHECK_EQ((int)out[2].timestamp, 9);
}

static void test_findLast() {
    RingBuffer<16> rb;
    rb.push(mk("s", "m", 1.0f, 1));
    rb.push(mk("s", "x", 2.0f, 2));   // different metric
    rb.push(mk("s", "m", 3.0f, 3));   // most recent "m"

    SensorReading out;
    CHECK(rb.findLast("s", "m", out));
    CHECK_EQ((int)out.value, 3);
    CHECK(!rb.findLast("s", "absent", out));
}

static void test_collectMetricSeries_chronological() {
    RingBuffer<16> rb;
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

int main() {
    RUN(test_push_and_copy_order);
    RUN(test_overflow_keeps_most_recent);
    RUN(test_copyRecent_fromTs_filter);
    RUN(test_findLast);
    RUN(test_collectMetricSeries_chronological);
    return SUMMARY();
}
