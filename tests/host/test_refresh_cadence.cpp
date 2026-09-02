// Host unit tests for src/web/RefreshCadence.h
//   - the clock's demand lands on a minute boundary from every second of the
//     minute, which is the property the whole design turns on
//   - the data prediction's three age bands, and its two "nothing is coming"
//     escapes
//   - the interaction: a data delay that falls a whisker short of the boundary
//     must not steal the alignment
//   - an unsynced device, where the clock is not drawn and must not pin
//
// The alignment claim used to live in a commit message and a scratch harness.
// It lives here now so CI re-checks it.
#include <stdint.h>

// Set the knobs before the header so this file states the configuration it is
// testing rather than inheriting whatever setup.h happens to say.
#define KINDLE_REFRESH_SEC           300
#define KINDLE_REFRESH_MIN_SEC       60
#define KINDLE_DATA_PERIOD_SEC       60
#define KINDLE_FOLLOW_DATA           1
#define KINDLE_CLOCK_PIN_REFRESH     1
#define KINDLE_CLOCK_SYNC_GUARD_SEC  20

#include "src/web/RefreshCadence.h"
#include "check.h"

// A timestamp on a minute boundary, comfortably past KINDLE_MIN_REAL_TS.
static const uint32_t BOUNDARY = 1000000020u;

// ---------------------------------------------------------------------------
// The property the design turns on
// ---------------------------------------------------------------------------
// A page reloading at :58 renders the minute current at :58 and holds it until
// :58 of the next — showing the previous minute for 58 of every 60 seconds.
// Landing on :00 is what makes the displayed minute change when the minute
// does, so it has to hold from wherever in the minute the page was loaded.
static void test_alignment_from_every_second() {
    for (uint32_t s = 0; s < 60; s++) {
        const uint32_t now = BOUNDARY + s;
        const uint32_t d   = kdRefreshDelaySec(BOUNDARY, now, true);
        CHECK((now + d) % 60u == 0u);
        // And never so soon after rendering that the panel flashes twice in a
        // breath, nor beyond the ceiling.
        CHECK(d >= (uint32_t)KINDLE_CLOCK_SYNC_GUARD_SEC);
        CHECK(d <= (uint32_t)KINDLE_REFRESH_SEC);
    }
}

// The guard is what stops a near boundary turning into an instant second
// repaint: at :45..:59 the next boundary is under 20 s away, so the one after
// is taken instead.
static void test_sync_guard_skips_a_near_boundary() {
    CHECK(kdClockDelaySec(BOUNDARY + 41u) == 19u + 60u);   // 19 s away -> skip
    CHECK(kdClockDelaySec(BOUNDARY + 40u) == 20u);         // 20 s away -> take it
    CHECK(kdClockDelaySec(BOUNDARY + 59u) == 1u  + 60u);
    CHECK(kdClockDelaySec(BOUNDARY)       == 60u);
}

// ---------------------------------------------------------------------------
// The data prediction, with the clock out of the way
// ---------------------------------------------------------------------------
static void test_data_bands_unpinned() {
    // clockShown = false is the unsynced-device path; it is also the cleanest
    // way to see the data logic on its own.
    const uint32_t T = BOUNDARY;

    // Fresh: land just after the next reading is due, but never under the floor.
    CHECK(kdRefreshDelaySec(T, T, false) == 64u);
    CHECK(kdRefreshDelaySec(T, T + 30u, false) == 60u);    // 34 raised to the floor

    // Late by less than a period: one missed post is ordinary, look again soon.
    CHECK(kdRefreshDelaySec(T, T + 90u, false) == 60u);

    // Two periods with nothing: the source is down, back off to the ceiling.
    CHECK(kdRefreshDelaySec(T, T + 7200u, false) == 300u);

    // No reading at all, and a clock that has stepped backwards behind the
    // reading — both are "we know nothing", both take the ceiling.
    CHECK(kdRefreshDelaySec(0, T, false) == 300u);
    CHECK(kdRefreshDelaySec(T + 5u, T, false) == 300u);
}

// ---------------------------------------------------------------------------
// The interaction
// ---------------------------------------------------------------------------
// The bug this guards: at :58 the boundary is 2 s away, the guard pushes the
// clock's request to 62, and the data path — floored at 60 — used to undercut
// it by two seconds and lock the page permanently to the :58 offset. Same
// repaint cost, clock wrong 58 seconds a minute.
static void test_data_must_be_meaningfully_earlier_to_win() {
    const uint32_t at58 = BOUNDARY + 58u;
    CHECK(kdRefreshDelaySec(BOUNDARY, at58, true) == 62u);
    CHECK((at58 + kdRefreshDelaySec(BOUNDARY, at58, true)) % 60u == 0u);
}

// The picker is where two bugs lived, so it is exercised on its own values
// rather than only through the cases the default configuration can reach.
static void test_picker_prefers_alignment_over_a_hair() {
    // A data delay of a minute or more is asking for the same cadence the
    // clock is; the clock's version lands on the boundary, so it wins even
    // when it is nominally later.
    CHECK(kdPickDelay(60u, 62u) == 62u);    // the :58 case
    CHECK(kdPickDelay(60u, 79u) == 79u);    // the :41 case
    CHECK(kdPickDelay(60u, 66u) == 66u);    // the :54 case
    CHECK(kdPickDelay(300u, 60u) == 60u);   // a dead node, clock still pinned

    // A genuinely sub-minute cadence is a different request, and wins when it
    // is earlier.
    CHECK(kdPickDelay(24u, 60u) == 24u);
    CHECK(kdPickDelay(24u, 10u) == 10u);    // unless the clock is earlier still

    // With no clock on the page the data is unopposed, ceiling included.
    CHECK(kdPickDelay(300u, KD_NO_CLOCK_DEMAND) == 300u);
    CHECK(kdPickDelay(64u,  KD_NO_CLOCK_DEMAND) == 64u);
}

// At the default settings the clock wins everywhere. That is the honest claim
// about KINDLE_FOLLOW_DATA: its shape is right, it is simply not the binding
// constraint while a minute clock is on the page.
static void test_the_clock_is_the_binding_constraint_at_defaults() {
    for (uint32_t s = 0; s < 60; s++) {
        const uint32_t now = BOUNDARY + s;
        CHECK(kdRefreshDelaySec(BOUNDARY, now, true) == kdClockDelaySec(now));
    }
}

// ---------------------------------------------------------------------------
// An unsynced device
// ---------------------------------------------------------------------------
// The page prints "clock not set" rather than a time, so there is no clock to
// keep honest and pinning would defeat the backoff entirely: a device with a
// dead node and no NTP would repaint every minute forever.
static void test_unsynced_device_backs_off() {
    const uint32_t T = BOUNDARY;
    CHECK(kdRefreshDelaySec(T, T + 7200u, true)  == 60u);    // clock shown: pinned
    CHECK(kdRefreshDelaySec(T, T + 7200u, false) == 300u);   // no clock: backs off
}

// ---------------------------------------------------------------------------
// The runtime knobs
// ---------------------------------------------------------------------------
// These three were preprocessor conditions while the settings page offered them
// as a form, so the panel saved values that nothing read. The point of these
// checks is not the arithmetic — that is covered above — but that passing a
// KdCadence changes the answer at all.
static void test_cadence_defaults_match_the_macros() {
    const uint32_t T = BOUNDARY;
    const KdCadence def;   // must be indistinguishable from the old behaviour
    CHECK_EQ((long)def.refreshSec, (long)KINDLE_REFRESH_SEC);
    for (uint32_t off = 0; off < 3600u; off += 137u)
        CHECK_EQ((long)kdRefreshDelaySec(T, T + off, true),
                 (long)kdRefreshDelaySec(T, T + off, true, def));
}

static void test_refresh_ceiling_is_honoured() {
    const uint32_t T = BOUNDARY;

    KdCadence slow;                       // a reader that should flash rarely
    slow.refreshSec      = 1800;
    slow.clockPinRefresh = false;         // or the clock pins it to a minute
    CHECK_EQ((long)kdRefreshDelaySec(T, T + 7200u, false, slow), 1800L);
    CHECK_EQ((long)kdRefreshDelaySec(0, T, false, slow), 1800L);

    KdCadence fast;
    fast.refreshSec      = 120;
    fast.clockPinRefresh = false;
    CHECK_EQ((long)kdRefreshDelaySec(T, T + 7200u, false, fast), 120L);
}

static void test_clock_pinning_can_be_turned_off() {
    // :58 — the case the picker exists for. With pinning on, the clock's
    // aligned 62 s wins; with it off, the data path decides alone.
    const uint32_t at58 = BOUNDARY + 58u;

    KdCadence on;
    CHECK_EQ((long)kdRefreshDelaySec(BOUNDARY, at58, true, on), 62L);

    KdCadence off;
    off.clockPinRefresh = false;
    CHECK(kdRefreshDelaySec(BOUNDARY, at58, true, off) != 62L);
    // and the result no longer lands on a minute boundary by construction
    CHECK_EQ((long)kdRefreshDelaySec(BOUNDARY, at58, true, off), 60L);
}

static void test_follow_data_can_be_turned_off() {
    const uint32_t T = BOUNDARY;

    KdCadence off;
    off.followData      = false;
    off.clockPinRefresh = false;
    // Fresh data would have asked for ~64 s; with following off every case is
    // the ceiling.
    CHECK_EQ((long)kdRefreshDelaySec(T, T, false, off), (long)KINDLE_REFRESH_SEC);
    CHECK_EQ((long)kdRefreshDelaySec(T, T + 30u, false, off), (long)KINDLE_REFRESH_SEC);

    KdCadence on;
    on.clockPinRefresh = false;
    CHECK_EQ((long)kdRefreshDelaySec(T, T, false, on), 64L);
}

// A ceiling below the floor would invert the clamp — d is raised to the floor
// and then lowered to the ceiling, so every case would return the floor.
static void test_clamp_keeps_the_ceiling_above_the_floor() {
    KdCadence c;
    c.refreshSec = 1;
    c.clamp();
    CHECK(c.refreshSec >= (uint32_t)KINDLE_REFRESH_MIN_SEC);

    c.refreshSec = 999999;
    c.clamp();
    CHECK_EQ((long)c.refreshSec, 86400L);
}

int main() {
    RUN(test_cadence_defaults_match_the_macros);
    RUN(test_refresh_ceiling_is_honoured);
    RUN(test_clock_pinning_can_be_turned_off);
    RUN(test_follow_data_can_be_turned_off);
    RUN(test_clamp_keeps_the_ceiling_above_the_floor);
    RUN(test_alignment_from_every_second);
    RUN(test_sync_guard_skips_a_near_boundary);
    RUN(test_data_bands_unpinned);
    RUN(test_data_must_be_meaningfully_earlier_to_win);
    RUN(test_picker_prefers_alignment_over_a_hair);
    RUN(test_the_clock_is_the_binding_constraint_at_defaults);
    RUN(test_unsynced_device_backs_off);
    return SUMMARY();
}
