// Host unit tests for src/power/BatteryModel.h
//
// The claim worth testing here is not that the arithmetic is right — it is
// that the model REFUSES to answer when it cannot. A remaining-life figure is
// believed by whoever reads it, so every path that would invent one from too
// little data has its own case below: too few days, a flat trace, a rising
// one, a slope inside the noise floor.
//
// The knobs are set before the header, so this file states the configuration
// it is testing rather than inheriting the firmware's.
#include <stdint.h>

#define BATT_CUTOFF_MV   3400
#define BATT_FULL_MV     4200
#define BATT_MIN_POINTS  5
#define BATT_WARN_PCT    10
#define BATT_WARN_DAYS   7
#define BATT_MAX_DAYS    365

#include "src/power/BatteryModel.h"
#include "check.h"

static const uint32_t DAY  = 86400u;
// A Monday well past BATT_MIN_REAL_TS, on a day boundary.
static const uint32_t BASE = 1750000000u / DAY * DAY;

// ---------------------------------------------------------------------------
// State of charge
// ---------------------------------------------------------------------------
static void test_percent_endpoints() {
    CHECK_EQ((int)batteryPercent(4200), 100);
    CHECK_EQ((int)batteryPercent(4300), 100);   // above full: clamp, not wrap
    CHECK_EQ((int)batteryPercent(BATT_CUTOFF_MV), 0);
    CHECK_EQ((int)batteryPercent(3000), 0);
    CHECK_EQ((int)batteryPercent(0),    0);

    // The knee points the curve is built from come back exactly.
    CHECK_EQ((int)batteryPercent(4060), 90);
    CHECK_EQ((int)batteryPercent(3820), 50);
    CHECK_EQ((int)batteryPercent(3600), 10);
}

// Non-increasing across the whole range, with no step of more than a few
// points. A gap in the table or a reversed pair would show up here and
// nowhere else — the endpoints above would still pass.
static void test_percent_monotonic() {
    int prev = 101;
    for (int mv = 4300; mv >= 3000; mv--) {
        const int p = batteryPercent((uint16_t)mv);
        CHECK(p <= prev);
        CHECK(p >= 0 && p <= 100);
        CHECK(prev - p <= 3);      // no cliff between table rows
        prev = p;
    }
    CHECK_EQ(prev, 0);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
static void test_history_keeps_daily_minimum() {
    BatteryHistory h;
    batteryHistoryAdd(h, BASE + 3600, 3900);
    CHECK_EQ((int)h.count, 1);
    CHECK_EQ((int)h.mv[0], 3900);

    batteryHistoryAdd(h, BASE + 7200, 3950);   // higher: ignored
    CHECK_EQ((int)h.mv[0], 3900);

    batteryHistoryAdd(h, BASE + 10800, 3850);  // lower: this is the day's figure
    CHECK_EQ((int)h.mv[0], 3850);
    CHECK_EQ((int)h.count, 1);                 // still one day
}

static void test_history_ignores_unusable_clocks() {
    BatteryHistory h;
    batteryHistoryAdd(h, 0, 3900);             // never set
    batteryHistoryAdd(h, 12345, 3900);         // uptime seconds, not an epoch
    CHECK_EQ((int)h.count, 0);

    batteryHistoryAdd(h, BASE, 3900);
    batteryHistoryAdd(h, BASE - DAY, 3800);    // clock jumped backwards
    CHECK_EQ((int)h.count, 1);
    CHECK_EQ((int)h.mv[0], 3900);
    CHECK_EQ((long long)h.lastDay, (long long)(BASE / DAY));

    batteryHistoryAdd(h, BASE + DAY, 0);       // no reading is not a reading
    CHECK_EQ((int)h.count, 1);
}

// A day the node was unreachable stays in the array as a hole. Closing it up
// would compress the time axis and make the drain look steeper than it is.
static void test_history_gaps_hold_their_place() {
    BatteryHistory h;
    batteryHistoryAdd(h, BASE,           3900);
    batteryHistoryAdd(h, BASE + 3 * DAY, 3870);

    CHECK_EQ((int)h.count, 4);
    CHECK_EQ((int)h.mv[0], 3870);   // newest
    CHECK_EQ((int)h.mv[1], 0);      // the two days missed
    CHECK_EQ((int)h.mv[2], 0);
    CHECK_EQ((int)h.mv[3], 3900);   // oldest
}

static void test_history_long_absence_resets() {
    BatteryHistory h;
    for (int d = 0; d < 10; d++)
        batteryHistoryAdd(h, BASE + (uint32_t)d * DAY, (uint16_t)(3900 - d));
    CHECK_EQ((int)h.count, 10);

    // Away longer than the window: nothing kept is relevant, so start over
    // rather than fit a line across the hole.
    batteryHistoryAdd(h, BASE + 40 * DAY, 3700);
    CHECK_EQ((int)h.count, 1);
    CHECK_EQ((int)h.mv[0], 3700);
    for (int i = 1; i < BatteryHistory::CAP; i++) CHECK_EQ((int)h.mv[i], 0);
}

static void test_history_never_exceeds_capacity() {
    BatteryHistory h;
    for (int d = 0; d < 60; d++)
        batteryHistoryAdd(h, BASE + (uint32_t)d * DAY, (uint16_t)(4200 - d));
    CHECK_EQ((int)h.count, (int)BatteryHistory::CAP);
    CHECK_EQ((int)h.mv[0], 4200 - 59);                       // newest
    CHECK_EQ((int)h.mv[BatteryHistory::CAP - 1], 4200 - 59 + (BatteryHistory::CAP - 1));
}

// ---------------------------------------------------------------------------
// Extrapolation — the answers
// ---------------------------------------------------------------------------

/// Fill `h` with `days` daily minima falling by `dropPerDay` from `startMv`.
static void drain(BatteryHistory& h, int days, int startMv, int dropPerDay) {
    for (int d = 0; d < days; d++)
        batteryHistoryAdd(h, BASE + (uint32_t)d * DAY,
                          (uint16_t)(startMv - dropPerDay * d));
}

static void test_days_left_on_a_clean_slope() {
    BatteryHistory h;
    drain(h, 14, 3900, 2);          // 2 mV/day, newest reading is 3874

    // 474 mV of headroom above the 3400 cutoff, shed at 2 mV a day.
    CHECK_EQ((int)batteryDaysLeft(h, 3874), 237);

    // The estimate follows the voltage it is given, not the last one stored:
    // the caller passes the live reading, which may be hours newer.
    CHECK_EQ((int)batteryDaysLeft(h, 3600), 100);
}

static void test_days_left_is_capped() {
    BatteryHistory h;
    drain(h, 14, 3900, 1);          // 487 mV of headroom at 1 mV/day = 487 days
    CHECK_EQ((int)batteryDaysLeft(h, 3887), BATT_MAX_DAYS);
}

static void test_at_or_below_cutoff_is_zero() {
    BatteryHistory h;
    drain(h, 14, 3900, 2);
    CHECK_EQ((int)batteryDaysLeft(h, BATT_CUTOFF_MV), 0);
    CHECK_EQ((int)batteryDaysLeft(h, 3300), 0);
}

// ---------------------------------------------------------------------------
// Extrapolation — the refusals
// ---------------------------------------------------------------------------
static void test_refuses_without_enough_history() {
    for (int days = 1; days < BATT_MIN_POINTS; days++) {
        BatteryHistory h;
        drain(h, days, 3900, 2);
        CHECK_EQ((int)batteryDaysLeft(h, 3900), -1);
    }
    // And with exactly the minimum, it answers.
    BatteryHistory h;
    drain(h, BATT_MIN_POINTS, 3900, 2);
    CHECK(batteryDaysLeft(h, 3892) > 0);
}

// A fortnight of slots with only four readings in them is four points, not
// fourteen — the gaps must not be counted towards the minimum.
static void test_gaps_do_not_count_as_history() {
    BatteryHistory h;
    for (int d = 0; d < 4; d++)
        batteryHistoryAdd(h, BASE + (uint32_t)(d * 3) * DAY, (uint16_t)(3900 - 6 * d));
    CHECK_EQ((int)h.count, 10);                     // ten slots spanned
    CHECK_EQ((int)batteryDaysLeft(h, 3882), -1);    // but only four readings
}

static void test_refuses_when_flat_or_rising() {
    {   // dead flat
        BatteryHistory h;
        drain(h, 14, 3900, 0);
        CHECK_EQ((int)batteryDaysLeft(h, 3900), -1);
    }
    {   // charging, or warming up after a cold night
        BatteryHistory h;
        for (int d = 0; d < 14; d++)
            batteryHistoryAdd(h, BASE + (uint32_t)d * DAY, (uint16_t)(3800 + d));
        CHECK_EQ((int)batteryDaysLeft(h, 3813), -1);
    }
    {   // falling, but by less than half a millivolt a day: inside the noise,
        // and the 3,000-day figure it would otherwise produce is nonsense.
        BatteryHistory h;
        for (int d = 0; d < 14; d++)
            batteryHistoryAdd(h, BASE + (uint32_t)d * DAY, (uint16_t)(3900 - d / 4));
        CHECK_EQ((int)batteryDaysLeft(h, 3897), -1);
    }
}

static void test_empty_history_refuses() {
    BatteryHistory h;
    CHECK_EQ((int)batteryDaysLeft(h, 3900), -1);
}

// ---------------------------------------------------------------------------
// The warning
// ---------------------------------------------------------------------------
// Either condition alone fires. That matters because the day count is absent
// far more often than people expect, and a node whose history was lost still
// has to warn before it dies.
static void test_warning_conditions() {
    CHECK(!batteryShouldWarn(80, -1));
    CHECK(!batteryShouldWarn(80, 200));

    CHECK(batteryShouldWarn(BATT_WARN_PCT, -1));        // percentage alone
    CHECK(batteryShouldWarn(BATT_WARN_PCT - 1, 300));
    CHECK(batteryShouldWarn(90, BATT_WARN_DAYS));       // day count alone
    CHECK(batteryShouldWarn(90, 0));

    CHECK(!batteryShouldWarn(BATT_WARN_PCT + 1, BATT_WARN_DAYS + 1));

    // -1 means "unknown", and unknown must never be read as "zero days left".
    CHECK(!batteryShouldWarn(100, -1));
}

int main() {
    RUN(test_percent_endpoints);
    RUN(test_percent_monotonic);
    RUN(test_history_keeps_daily_minimum);
    RUN(test_history_ignores_unusable_clocks);
    RUN(test_history_gaps_hold_their_place);
    RUN(test_history_long_absence_resets);
    RUN(test_history_never_exceeds_capacity);
    RUN(test_days_left_on_a_clean_slope);
    RUN(test_days_left_is_capped);
    RUN(test_at_or_below_cutoff_is_zero);
    RUN(test_refuses_without_enough_history);
    RUN(test_gaps_do_not_count_as_history);
    RUN(test_refuses_when_flat_or_rising);
    RUN(test_empty_history_refuses);
    RUN(test_warning_conditions);
    return SUMMARY();
}
