// ============================================================================
// src/power/BatteryModel.h
//
// Turning a battery node's reported voltage into the two things a human can
// act on: how full it is, and how long before it needs charging.
//
// WHY THIS LIVES ON THE COLLECTOR
// -------------------------------
// The node sends millivolts and nothing else. It has no history — it deep
// sleeps between wakes and its RAM does not survive — and giving it one would
// mean writing to flash every day on a device whose whole purpose is to not
// spend energy. The collector is mains powered, already keeps a filesystem,
// and is the thing with a screen. So the node measures and the collector
// concludes, which is also why this file has no Arduino in it and is tested on
// the build host.
//
// WHY THE DAILY MINIMUM
// ---------------------
// A lithium cell's terminal voltage moves with load and with temperature, and
// a node on an unheated balcony sees several tenths of a volt of daily swing
// that has nothing to do with charge. Recording one figure per day — the
// lowest seen, which is the one measured under the radio's load — takes most
// of that out. It is not calibration, but it turns a noisy trace into one a
// straight line can be fitted to.
//
// WHAT THE ESTIMATE IS AND IS NOT
// -------------------------------
// It is a linear extrapolation of the last fortnight to the cutoff voltage. A
// lithium discharge curve is not linear, and near the top of it — the long
// flat stretch from 4.1 V to 3.9 V — the slope is so shallow that the estimate
// is dominated by measurement noise. That is exactly why batteryDaysLeft()
// refuses to answer instead of answering badly:
//
//   • fewer than BATT_MIN_POINTS days of history          → -1
//   • a slope that is flat or rising                      → -1
//
// A dash on the dashboard is a true statement. "412 days" derived from four
// readings that differ by two millivolts is not, and it is worse than the dash
// because a reader believes it.
//
// The warning is therefore not built on the estimate alone: crossing
// BATT_WARN_PCT fires on its own, so a node whose history was lost still warns
// before it dies.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
// #ifndef so a host test can state the configuration it is testing rather than
// inheriting whatever the firmware happens to be built with — the same pattern
// as src/web/RefreshCadence.h.

/// The voltage at which the node stops working, which is what "empty" means
/// here — not the cell's own 3.0 V floor.
///
/// The node runs from a 3.3 V LDO, and an LDO cannot regulate once its input
/// falls to within its dropout of its output. At 3.4 V there is nothing left
/// to give, so the roughly 600 mAh of charge the cell still holds below this
/// point is unreachable and counting it would overstate the remaining life by
/// weeks.
#ifndef BATT_CUTOFF_MV
#  define BATT_CUTOFF_MV 3400
#endif

/// Full. A lithium cell charged to 4.2 V and left to settle sits a little
/// below it; the curve below starts there rather than at the charger's figure.
#ifndef BATT_FULL_MV
#  define BATT_FULL_MV 4200
#endif

/// Days of history before an extrapolation is offered at all.
#ifndef BATT_MIN_POINTS
#  define BATT_MIN_POINTS 5
#endif

/// Warn at or below this percentage.
#ifndef BATT_WARN_PCT
#  define BATT_WARN_PCT 10
#endif

/// Warn at or below this many days remaining — the "last week" the dashboard
/// is meant to give notice of.
#ifndef BATT_WARN_DAYS
#  define BATT_WARN_DAYS 7
#endif

/// Longest estimate we will print. Beyond a year the linear fit is fiction
/// dressed as arithmetic, and "365+" says the useful part of it.
#ifndef BATT_MAX_DAYS
#  define BATT_MAX_DAYS 365
#endif

// ---------------------------------------------------------------------------
// State of charge
// ---------------------------------------------------------------------------

/// Percentage full, from a piecewise-linear lithium discharge curve.
///
/// The knee points are for a 21700 cell under the light, bursty load this node
/// puts on it — nothing like a datasheet's 0.2C curve, which is why the flat
/// middle is flatter here. The bottom point is BATT_CUTOFF_MV rather than the
/// cell's own floor, so 0% means "the node is about to stop", which is the
/// question being asked.
///
/// Returns 0..100, clamped at both ends.
static inline uint8_t batteryPercent(uint16_t mv) {
    // Descending by voltage. Second column is the percentage at that voltage.
    static const uint16_t CURVE[][2] = {
        {4200, 100}, {4060, 90}, {3980, 80}, {3920, 70}, {3870, 60},
        {3820,  50}, {3790, 40}, {3750, 30}, {3700, 20}, {3600, 10},
        {BATT_CUTOFF_MV, 0},
    };
    const int N = (int)(sizeof(CURVE) / sizeof(CURVE[0]));

    if (mv >= CURVE[0][0])     return 100;
    if (mv <= CURVE[N - 1][0]) return 0;

    for (int i = 0; i < N - 1; i++) {
        const uint16_t hiV = CURVE[i][0],     loV = CURVE[i + 1][0];
        if (mv > loV && mv <= hiV) {
            const uint16_t hiP = CURVE[i][1], loP = CURVE[i + 1][1];
            // Integer interpolation, rounded. The span is never zero: the
            // table is strictly descending, which the static_assert-ish check
            // in the host test re-verifies.
            const int32_t num = (int32_t)(mv - loV) * (int32_t)(hiP - loP);
            const int32_t den = (int32_t)(hiV - loV);
            return (uint8_t)(loP + (num + den / 2) / den);
        }
    }
    return 0;   // unreachable while the table is descending
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

/// A fortnight of daily minima, newest first.
///
/// Small enough (33 bytes) that one per node can be held in RAM and written to
/// the filesystem once a day. Gaps — days the node was unreachable — are
/// stored as zero and kept in place rather than closed up, because the slope
/// depends on how far apart the readings are and a closed gap would flatten
/// it.
struct BatteryHistory {
    static constexpr uint8_t CAP = 14;

    uint32_t lastDay;    ///< epoch/86400 of mv[0]; 0 when empty
    uint8_t  count;      ///< occupied slots, 1..CAP (includes gap slots)
    uint16_t mv[CAP];    ///< mv[0] newest .. mv[count-1] oldest; 0 = no data

    BatteryHistory() { memset(this, 0, sizeof(*this)); }
};

/// A timestamp below this is a device whose clock has never been set — the
/// same threshold the Kindle renderer uses to decide not to draw a date.
#ifndef BATT_MIN_REAL_TS
#  define BATT_MIN_REAL_TS 1000000000u
#endif

/// Fold one reading into the history.
///
/// Called on every arriving battery voltage, not once a day: it keeps the
/// running minimum for the current day itself, so the caller does not need a
/// timer. Readings with no usable clock are dropped rather than bucketed into
/// day zero, and a clock that jumps backwards is ignored for the same reason —
/// both would corrupt the day axis the slope is measured against.
static inline void batteryHistoryAdd(BatteryHistory& h, uint32_t epoch, uint16_t mv) {
    if (epoch < BATT_MIN_REAL_TS || mv == 0) return;

    const uint32_t day = epoch / 86400u;

    if (h.count == 0) {
        h.lastDay = day;
        h.count   = 1;
        h.mv[0]   = mv;
        return;
    }
    if (day == h.lastDay) {
        if (mv < h.mv[0] || h.mv[0] == 0) h.mv[0] = mv;   // daily minimum
        return;
    }
    if (day < h.lastDay) return;                          // clock went backwards

    const uint32_t gap   = day - h.lastDay;
    if (gap >= BatteryHistory::CAP) {
        // The whole window is stale — a node back after a fortnight away, or a
        // clock that was just set for the first time. Start over rather than
        // fit a line across the hole.
        memset(h.mv, 0, sizeof(h.mv));
        h.count = 1;
    } else {
        const int shift = (int)gap;
        for (int i = BatteryHistory::CAP - 1; i >= shift; i--)
            h.mv[i] = h.mv[i - shift];
        for (int i = 0; i < shift; i++) h.mv[i] = 0;       // the days missed
        const int c = (int)h.count + shift;
        h.count = (uint8_t)(c > BatteryHistory::CAP ? (int)BatteryHistory::CAP : c);
    }
    h.lastDay = day;
    h.mv[0]   = mv;
}

// ---------------------------------------------------------------------------
// Extrapolation
// ---------------------------------------------------------------------------

/// Days until the node reaches BATT_CUTOFF_MV, or -1 if that cannot be said.
///
/// Least squares over the daily minima, in integer arithmetic — the inputs are
/// small enough that nothing here needs a float, and on the ESP32-C3 there is
/// no hardware float to use anyway.
///
/// Returns -1 when the answer would be invented rather than measured: too few
/// days, or a slope that is flat or rising. A charged node reads as rising and
/// gets -1, which is correct — it has no drain to extrapolate yet.
static inline int16_t batteryDaysLeft(const BatteryHistory& h, uint16_t nowMv) {
    if (h.count < BATT_MIN_POINTS) return -1;

    // x counts days forward from the oldest slot, so a draining battery gives
    // a negative slope and the sign reads the way it looks on a chart.
    int32_t n = 0, sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (int i = 0; i < h.count; i++) {
        if (h.mv[i] == 0) continue;              // a day with no reading
        const int32_t x = (int32_t)(h.count - 1 - i);
        const int32_t y = (int32_t)h.mv[i];
        n++; sx += x; sy += y; sxy += x * y; sxx += x * x;
    }
    if (n < BATT_MIN_POINTS) return -1;

    const int32_t den = n * sxx - sx * sx;       // > 0 once two x's differ
    if (den <= 0) return -1;
    const int32_t num = n * sxy - sx * sy;       // slope = num/den, mV per day

    // Flat or rising: nothing to extrapolate. The threshold is half a
    // millivolt per day, below which the fit is reading its own noise.
    if (num >= 0 || 2 * (-(int64_t)num) < (int64_t)den) return -1;

    const int32_t headroom = (int32_t)nowMv - BATT_CUTOFF_MV;
    if (headroom <= 0) return 0;

    const int64_t days = ((int64_t)headroom * den) / (-(int64_t)num);
    if (days >= BATT_MAX_DAYS) return BATT_MAX_DAYS;
    return (int16_t)days;
}

/// Whether the dashboard should show the warning badge.
///
/// Either condition alone is enough. The percentage is the one that always
/// works; the day count is the one that gives notice, and it is absent
/// (`days < 0`) far more often than people expect — see batteryDaysLeft().
static inline bool batteryShouldWarn(uint8_t pct, int16_t days) {
    return pct <= BATT_WARN_PCT || (days >= 0 && days <= BATT_WARN_DAYS);
}
