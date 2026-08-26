// ============================================================================
// src/web/RefreshCadence.h
//
// How long the Kindle dashboard waits before reloading itself.
//
// Split out of KindleDashboard.cpp so it can be tested on the host: the rest
// of that file needs Arduino, AsyncWebServer and FreeRTOS, and this does not
// need any of them. tests/host/test_refresh_cadence.cpp includes this header
// directly, which is what makes the exhaustive alignment claim in the docs
// something CI re-checks rather than something a commit message asserts.
//
// The knobs are documented in KindleDashboard.h; the #ifndef fallbacks here
// exist so a host test can set them without dragging in setup.h.
// ============================================================================
#pragma once

#include <stdint.h>

#ifndef KINDLE_REFRESH_SEC
#  define KINDLE_REFRESH_SEC 300
#endif
#ifndef KINDLE_REFRESH_MIN_SEC
#  define KINDLE_REFRESH_MIN_SEC 60
#endif
#ifndef KINDLE_DATA_PERIOD_SEC
#  define KINDLE_DATA_PERIOD_SEC 60
#endif
#ifndef KINDLE_FOLLOW_DATA
#  define KINDLE_FOLLOW_DATA 1
#endif
#ifndef KINDLE_CLOCK_PIN_REFRESH
#  define KINDLE_CLOCK_PIN_REFRESH 1
#endif

#if KINDLE_REFRESH_MIN_SEC > KINDLE_REFRESH_SEC
#  error "KINDLE_REFRESH_MIN_SEC exceeds KINDLE_REFRESH_SEC: the floor is above the ceiling"
#endif

// Never reload sooner than this after rendering, whatever the clock wants.
// The clock's demand is aimed at a minute boundary, and when that boundary is
// nearly here the choice is between a jarring near-instant second flash and
// waiting for the following one. 20 s is the line: below it, take the next
// boundary instead.
//
// This is separate from KINDLE_REFRESH_MIN_SEC, which floors the *data*
// path. The clock cannot honour a 60 s floor and still align to the minute
// from a cold load, so the first reload after a fresh load may come in 20-79 s;
// after that the cadence is a steady 60 s on the minute.
#ifndef KINDLE_CLOCK_SYNC_GUARD_SEC
#  define KINDLE_CLOCK_SYNC_GUARD_SEC 20
#endif

// A timestamp below this is a device whose clock has never been set.
#ifndef KINDLE_MIN_REAL_TS
#  define KINDLE_MIN_REAL_TS 1000000000u
#endif

// ---------------------------------------------------------------------------
// The clock's demand
// ---------------------------------------------------------------------------
// Aimed at the next minute boundary rather than "60 seconds from now", and
// that is not cosmetic. The clock is rendered server-side, so a page reloading
// at :58 of each minute renders the minute current at :58 and holds it until
// :58 of the next — showing the previous minute for 58 seconds out of every
// 60. Landing on :00 makes the displayed minute change when the minute does.
inline uint32_t kdClockDelaySec(uint32_t now) {
    uint32_t toBoundary = 60u - (now % 60u);              // 1..60
    if (toBoundary < (uint32_t)KINDLE_CLOCK_SYNC_GUARD_SEC) toBoundary += 60u;
    return toBoundary;
}

// The value clockWants takes when there is no clock to keep honest.
static const uint32_t KD_NO_CLOCK_DEMAND = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Which of the two demands to obey
// ---------------------------------------------------------------------------
// Not a min(), and the reason is worth stating because two separate bugs came
// out of getting it wrong, at opposite ends of the same minute.
//
// The clock's request is the one carrying the minute alignment, and alignment
// is the entire reason the clock has a request at all: landing at :58 instead
// of :00 costs exactly the same repaint but shows the previous minute for 58
// of every 60 seconds.
//
// A plain min() lost it twice. At :58 the boundary is 2 s away, the sync guard
// pushes the clock to 62, and a data path floored at 60 undercut it by two
// seconds — locking the page permanently to the :58 offset. Requiring the data
// to be five seconds earlier fixed that end and broke the other: from :41 to
// :54 the guard pushes the clock to 79..66, the data's 60 clears the
// five-second margin, and the alignment is stolen again.
//
// So the test is not "is the data earlier" but "does the data genuinely need a
// faster cadence than one repaint a minute". Anything asking for 60 s or more
// is asking for the same thing the clock is, and the clock's version is the
// one that lands on the minute.
inline uint32_t kdPickDelay(uint32_t dataWants, uint32_t clockWants) {
    if (clockWants == KD_NO_CLOCK_DEMAND) return dataWants;
    const bool dataIsGenuinelyFaster = (dataWants + 5u < 60u) &&
                                       (dataWants + 5u < clockWants);
    return dataIsGenuinelyFaster ? dataWants : clockWants;
}

// ---------------------------------------------------------------------------
// The delay to put in the meta refresh
// ---------------------------------------------------------------------------
// Two demands, and the smaller wins — with one qualification below.
//
// The data demand is a prediction, not a subscription: nothing can push to the
// reader, so the page aims its own reload just after the next reading is due.
// The three cases exist because "no reading yet" and "the node died" both look
// like "the data is old", and neither should make an e-ink panel flash every
// minute forever waiting for something that is not coming.
//
// `clockShown` must be false when the page is not drawing a clock — an unsynced
// device prints "clock not set" instead, and pinning the refresh to a minute
// boundary for a clock nobody can see is the very backoff failure the data
// cases above are shaped to avoid.
inline uint32_t kdRefreshDelaySec(uint32_t newestTs, uint32_t now, bool clockShown) {
#if KINDLE_CLOCK_PIN_REFRESH
    const uint32_t clockWants = clockShown ? kdClockDelaySec(now) : KD_NO_CLOCK_DEMAND;
#else
    (void)clockShown;
    const uint32_t clockWants = KD_NO_CLOCK_DEMAND;
#endif
    (void)now;   // unused when both the clock and the data path are off

#if KINDLE_FOLLOW_DATA
    if (newestTs != 0 && now >= newestTs) {
        const uint32_t age    = now - newestTs;
        const uint32_t period = KINDLE_DATA_PERIOD_SEC;

        uint32_t d;
        if (age < period) {
            // Land a few seconds after the reading is due rather than exactly
            // on it: arriving early means rendering the value we already show
            // and paying for a repaint that changed nothing.
            d = period - age + 4u;
        } else if (age < period * 2u) {
            // Late, but one missed post is ordinary. Look again soon.
            d = KINDLE_REFRESH_MIN_SEC;
        } else {
            // Two periods with nothing is a source that is down. Back off to
            // the ceiling; the page already says the reading is stale, and
            // flashing at it will not bring the node back.
            return kdPickDelay((uint32_t)KINDLE_REFRESH_SEC, clockWants);
        }

        if (d < (uint32_t)KINDLE_REFRESH_MIN_SEC) d = KINDLE_REFRESH_MIN_SEC;
        if (d > (uint32_t)KINDLE_REFRESH_SEC)     d = KINDLE_REFRESH_SEC;
        return kdPickDelay(d, clockWants);
    }
#else
    (void)newestTs;
#endif
    return kdPickDelay((uint32_t)KINDLE_REFRESH_SEC, clockWants);
}
