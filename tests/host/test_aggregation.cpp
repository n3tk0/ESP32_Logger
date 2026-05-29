// Host unit tests for src/pipeline/AggregationEngine.{h,cpp}
//   - lttb()      downsampling (endpoint preservation, passthrough, bounds)
//   - bucket()    time-window reduction (raw/avg/min/max/sum)
//   - aggregate() combined bucket -> LTTB pipeline
//
// AggregationEngine depends only on <Arduino.h> + SensorTypes + <math.h>, so we
// compile its implementation straight into this translation unit.
#include "src/pipeline/AggregationEngine.cpp"
#include "check.h"

static SensorReading mk(uint32_t ts, float value) {
    return SensorReading::make(ts, "s", "t", "m", value, "u", QUALITY_GOOD);
}

// ---- lttb ------------------------------------------------------------------

static void test_lttb_passthrough_when_small() {
    SensorReading in[5];
    for (int i = 0; i < 5; i++) in[i] = mk((uint32_t)i, (float)i);
    SensorReading out[16];
    size_t n = AggregationEngine::lttb(in, 5, out, 10);
    CHECK_EQ(n, (size_t)5);
    bool same = true;
    for (int i = 0; i < 5; i++) if (out[i].value != (float)i) same = false;
    CHECK(same);
}

static void test_lttb_edge_counts() {
    SensorReading in[4];
    for (int i = 0; i < 4; i++) in[i] = mk((uint32_t)i, (float)i);
    SensorReading out[4];
    CHECK_EQ(AggregationEngine::lttb(in, 0, out, 4), (size_t)0);   // empty in
    CHECK_EQ(AggregationEngine::lttb(in, 4, out, 0), (size_t)0);   // zero out
    CHECK_EQ(AggregationEngine::lttb(in, 4, out, 1), (size_t)1);   // single point
}

static void test_lttb_downsample_keeps_endpoints() {
    const size_t N = 100;
    SensorReading in[N];
    for (size_t i = 0; i < N; i++) in[i] = mk((uint32_t)i, (float)(i * i % 37));
    SensorReading out[10];
    size_t n = AggregationEngine::lttb(in, N, out, 10);
    // LTTB yields AT MOST maxPoints (empty buckets can produce fewer) but must
    // always keep the first and last sample.
    CHECK(n >= 2 && n <= 10);
    CHECK_EQ((int)out[0].timestamp, 0);            // first preserved
    CHECK_EQ((int)out[n-1].timestamp, (int)(N-1)); // last preserved
}

// ---- bucket ----------------------------------------------------------------

static void test_bucket_raw_passthrough() {
    SensorReading in[3] = { mk(0,1), mk(1,2), mk(2,3) };
    SensorReading out[8];
    size_t n = AggregationEngine::bucket(in, 3, out, 8, BUCKET_RAW, AGG_AVG);
    CHECK_EQ(n, (size_t)3);
}

static void test_bucket_sum_per_window() {
    // 1-minute buckets (60s). Bucket0 = ts 0,10,20 ; bucket1 = ts 60,70.
    SensorReading in[5] = { mk(0,1), mk(10,2), mk(20,3), mk(60,4), mk(70,5) };
    SensorReading out[8];
    size_t n = AggregationEngine::bucket(in, 5, out, 8, BUCKET_1MIN, AGG_SUM);
    CHECK_EQ(n, (size_t)2);
    CHECK_EQ((int)out[0].value, 6);   // 1+2+3
    CHECK_EQ((int)out[1].value, 9);   // 4+5
}

static void test_bucket_min_max() {
    SensorReading in[4] = { mk(0,5), mk(10,2), mk(20,9), mk(30,7) };  // single bucket
    SensorReading out[8];
    size_t nmin = AggregationEngine::bucket(in, 4, out, 8, BUCKET_1MIN, AGG_MIN);
    CHECK_EQ(nmin, (size_t)1);
    CHECK_EQ((int)out[0].value, 2);
    size_t nmax = AggregationEngine::bucket(in, 4, out, 8, BUCKET_1MIN, AGG_MAX);
    CHECK_EQ(nmax, (size_t)1);
    CHECK_EQ((int)out[0].value, 9);
}

static void test_bucket_avg() {
    SensorReading in[4] = { mk(0,2), mk(10,4), mk(20,6), mk(30,8) };  // avg = 5
    SensorReading out[8];
    size_t n = AggregationEngine::bucket(in, 4, out, 8, BUCKET_1MIN, AGG_AVG);
    CHECK_EQ(n, (size_t)1);
    CHECK_EQ((int)out[0].value, 5);
}

// ---- aggregate -------------------------------------------------------------

static void test_aggregate_respects_bounds() {
    const size_t N = 300;
    SensorReading in[N];
    for (size_t i = 0; i < N; i++) in[i] = mk((uint32_t)(i * 5), (float)i);
    SensorReading out[64];
    size_t n = AggregationEngine::aggregate(in, N, out, 64,
                                            BUCKET_RAW, AGG_LTTB, /*maxPoints=*/50);
    CHECK(n <= (size_t)64);   // never overruns out buffer
    CHECK(n <= (size_t)50);   // LTTB cap honoured
}

int main() {
    RUN(test_lttb_passthrough_when_small);
    RUN(test_lttb_edge_counts);
    RUN(test_lttb_downsample_keeps_endpoints);
    RUN(test_bucket_raw_passthrough);
    RUN(test_bucket_sum_per_window);
    RUN(test_bucket_min_max);
    RUN(test_bucket_avg);
    RUN(test_aggregate_respects_bounds);
    return SUMMARY();
}
