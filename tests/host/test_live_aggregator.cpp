// Host unit tests for src/pipeline/LiveAggregator.cpp — the data log's sensor
// columns, one value per interval:
//   - each column by its own DatalogAgg: avg, min, max, last, sum
//   - a column with no mode given, or past the modes passed, is averaged
//   - an empty column is NAN, whatever its mode
//   - the value is the mode's at the close; driven the way StorageTask does
//   - the aggregation names (DatalogFormat.cpp) and their fallback
#include <math.h>
#include <string.h>

#include "src/storage/DatalogFormat.cpp"
#include "src/pipeline/LiveAggregator.cpp"
#include "check.h"

static SensorReading rd(const char* metric, float v, SensorQuality q = QUALITY_GOOD) {
    SensorReading r;
    strcpy(r.sensorId, "s");
    strcpy(r.sensorType, "bme280");
    strncpy(r.metric, metric, sizeof(r.metric) - 1);
    r.value = v;
    r.quality = q;
    return r;
}

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_each_column_by_its_mode() {
    LiveAggregator a;
    a.setIntervalSec(60);
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    CHECK(!a.take(1000, false, vals, &start));        // the first call starts the window
    const float xs[] = { 4.0f, 1.0f, 7.0f, 2.0f };
    for (int col = 0; col < 5; col++)
        for (float x : xs) a.feed(rd("temperature", x), col);
    const uint8_t modes[] = { DL_AGG_AVG, DL_AGG_MIN, DL_AGG_MAX, DL_AGG_LAST, DL_AGG_SUM };
    CHECK(!a.take(1059, false, vals, &start, modes, 5));   // not due yet
    CHECK(a.take(1060, false, vals, &start, modes, 5));
    CHECK_EQ(start, 1000);
    CHECK(near(vals[0], 3.5f));
    CHECK(near(vals[1], 1.0f));
    CHECK(near(vals[2], 7.0f));
    CHECK(near(vals[3], 2.0f));
    CHECK(near(vals[4], 14.0f));
    for (int col = 5; col < LiveAggregator::MAX_COLUMNS; col++) CHECK(isnan(vals[col]));
}

static void test_no_modes_is_the_average() {
    // What a column saved before modes existed gets, and what take() gives
    // a column the modes do not reach.
    LiveAggregator a;
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    a.take(0 + 1, false, vals, &start);
    a.feed(rd("humidity", 40), 0);
    a.feed(rd("humidity", 50), 0);
    a.feed(rd("humidity", 10), 3);
    a.feed(rd("humidity", 30), 3);
    CHECK(a.take(100, true, vals, &start));
    CHECK(near(vals[0], 45));
    CHECK(near(vals[3], 20));
    const uint8_t one[] = { DL_AGG_MAX };
    a.feed(rd("humidity", 40), 0);
    a.feed(rd("humidity", 50), 0);
    a.feed(rd("humidity", 10), 3);
    a.feed(rd("humidity", 30), 3);
    CHECK(a.take(200, true, vals, &start, one, 1));
    CHECK(near(vals[0], 50));          // its mode
    CHECK(near(vals[3], 20));          // past nModes: averaged
}

static void test_min_max_are_the_windows_own() {
    // Negative readings, and a window after one with larger values: the
    // extremes start over with each window rather than carrying across.
    LiveAggregator a;
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    const uint8_t modes[] = { DL_AGG_MIN, DL_AGG_MAX };
    a.take(1, false, vals, &start);
    a.feed(rd("temperature", -3), 0); a.feed(rd("temperature", -8), 0);
    a.feed(rd("temperature", -3), 1); a.feed(rd("temperature", -8), 1);
    CHECK(a.take(100, true, vals, &start, modes, 2));
    CHECK(near(vals[0], -8)); CHECK(near(vals[1], -3));
    a.feed(rd("temperature", 5), 0); a.feed(rd("temperature", 9), 0);
    a.feed(rd("temperature", 5), 1); a.feed(rd("temperature", 2), 1);
    CHECK(a.take(200, true, vals, &start, modes, 2));
    CHECK(near(vals[0], 5)); CHECK(near(vals[1], 5));
}

static void test_bad_readings_do_not_count() {
    LiveAggregator a;
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    const uint8_t modes[] = { DL_AGG_LAST, DL_AGG_MIN };
    a.take(1, false, vals, &start);
    a.feed(rd("temperature", 3), 0);
    a.feed(rd("temperature", NAN), 0);
    a.feed(rd("temperature", -40, QUALITY_ERROR), 0);
    a.feed(rd("temperature", -40, QUALITY_ERROR), 1);
    CHECK(a.take(100, true, vals, &start, modes, 2));
    CHECK(near(vals[0], 3));           // the last GOOD reading
    CHECK(isnan(vals[1]));             // only an error arrived: nothing
}

static void test_mode_change_applies_to_the_open_window() {
    // The same readings, window after window, closed under a different mode
    // each time: the value is the mode's at the close, whatever the column's
    // mode was while the readings arrived — nothing is fixed when a window
    // opens, and every statistic is kept for every column.
    LiveAggregator a;
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    a.take(1, false, vals, &start);
    const uint8_t order[] = { DL_AGG_AVG, DL_AGG_MAX, DL_AGG_MIN, DL_AGG_SUM, DL_AGG_LAST, DL_AGG_AVG };
    const float want[]    = { 1005.0f,    1010.0f,    1000.0f,    2010.0f,    1010.0f,     1005.0f };
    for (int w = 0; w < 6; w++) {
        a.feed(rd("pressure", 1000), 0);
        a.feed(rd("pressure", 1010), 0);
        CHECK(a.take(100u * (w + 2), true, vals, &start, &order[w], 1));
        CHECK(near(vals[0], want[w]));
    }
}

static void test_driven_as_storage_task_drives_it() {
    // take() on every pass, every second for three minutes: the first call
    // starts the window, and one closes every minute after it.
    LiveAggregator a;
    a.setIntervalSec(60);
    float vals[LiveAggregator::MAX_COLUMNS];
    uint32_t start = 0;
    const uint8_t mx[] = { DL_AGG_MAX };
    int rows = 0;
    for (uint32_t t = 5000; t < 5000 + 180; t++) {
        a.feed(rd("temperature", (float)(t % 7)), 0);
        if (a.take(t, false, vals, &start, mx, 1)) {
            rows++;
            CHECK(near(vals[0], 6));
        }
    }
    CHECK_EQ(rows, 2);                           // at 5060 and 5120; 5180 not reached
    // A reset (the columns changed) starts the next window over.
    a.reset();
    CHECK(!a.take(6000, false, vals, &start));
    a.feed(rd("temperature", 1), 0);
    CHECK(a.take(6060, false, vals, &start));
    CHECK_EQ(start, 6000);
}

static void test_agg_names() {
    const char* names[] = { "avg", "min", "max", "last", "sum" };
    for (uint8_t i = 0; i < DL_AGG_COUNT; i++) {
        CHECK_STREQ(datalogAggName(i), names[i]);
        CHECK_EQ(datalogAggFromName(names[i]), i);
    }
    // A column without "a", or with a word this firmware does not know
    // (a newer one's), is averaged — as every column was before.
    CHECK_EQ(datalogAggFromName(""), DL_AGG_AVG);
    CHECK_EQ(datalogAggFromName(nullptr), DL_AGG_AVG);
    CHECK_EQ(datalogAggFromName("median"), DL_AGG_AVG);
    CHECK_EQ(datalogAggFromName("MAX"), DL_AGG_AVG);
    CHECK_STREQ(datalogAggName(DL_AGG_COUNT), "avg");
    CHECK_STREQ(datalogAggName(200), "avg");
}

int main() {
    RUN(test_each_column_by_its_mode);
    RUN(test_no_modes_is_the_average);
    RUN(test_min_max_are_the_windows_own);
    RUN(test_bad_readings_do_not_count);
    RUN(test_mode_change_applies_to_the_open_window);
    RUN(test_driven_as_storage_task_drives_it);
    RUN(test_agg_names);
    return SUMMARY();
}
