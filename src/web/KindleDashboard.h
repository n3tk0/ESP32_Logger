// ============================================================================
// src/web/KindleDashboard.h
//
// GET /kindle — a dashboard for a 6" e-ink reader's browser.
//
// TARGET
// ------
// Kindle Paperwhite 4 — 10th generation, 2018, model PQ94WIF — and any other
// reader with the same 6" panel: 1072×1448 at 300 ppi.
//
// WHERE 600×800 COMES FROM
// ------------------------
// It is not a reported viewport. The page pins the layout width itself with
// its viewport meta, so the browser scales that width across the panel's 1072
// device px and the 1448 px of height works out to the same ratio: at the
// default 600 that is about 1.79× and roughly 810 CSS px of height, which is
// where the 800 px budget comes from. The width is a build-time knob — see
// KINDLE_PAGE_W below and GET /kindle/probe.
//
// WHY IT IS BUILT FOR AN OLD BROWSER ANYWAY
// -----------------------------------------
// The Experimental Browser is WebKit, but which WebKit depends on firmware.
// Older builds report a user agent in the 531–534 range, which is 2010–2011
// vintage: no fetch, no Promise, no ES6, no flexbox, no CSS grid. Firmware
// 5.16.4 modernised it on the 10th and 11th generation, so an up-to-date
// PQ94WIF would in fact handle a good deal more than this page uses.
//
// It is still built for the old one, because the cost of doing so is zero and
// the alternative is a page whose correctness depends on the reader's firmware
// version. So:
//
//   • the page is rendered server-side and ships zero JavaScript;
//   • layout is tables and blocks, because those work everywhere;
//   • the trend chart is inline SVG, drawn here as path data — no canvas,
//     no charting library, nothing to execute;
//   • it refreshes with <meta http-equiv="refresh">, not a timer.
//
// None of that is a sacrifice on this medium. A panel that repaints in full or
// not at all has no use for a script that updates part of itself.
//
// THE GREYS
// ---------
// The panel has 16 real grey levels and the page uses six tones plus two
// washes. An earlier version of this file argued for pure black and white on
// the grounds that mid greys dither into noise at this size; that was wrong.
// The dithering worth avoiding comes from gradients and from tones too close
// together, not from flat, well-separated fills.
//
// The two chart lines are still told apart by dash pattern as well as shade,
// because redundant coding costs nothing and survives a panel with its
// contrast turned down.
//
// REFRESH CADENCE
// ---------------
// Every full page load repaints the whole panel, which flashes and costs
// battery. KINDLE_REFRESH_SEC defaults to 300: fast enough that the reading
// on the shelf is current, slow enough not to strobe. There is no partial
// update available to a web page.
// ============================================================================
#pragma once

#include "../setup.h"

// ---------------------------------------------------------------------------
// Layout width
// ---------------------------------------------------------------------------
// The width the page declares in its viewport meta, and the unit every size in
// the stylesheet is expressed in. 600 is the default and what all the tuning
// was done at; every other value is that layout multiplied by PAGE_W/600 and
// rounded, so the proportions are identical and only the pixel grid changes.
//
// WHY THIS IS A KNOB AND NOT A CONSTANT
// -------------------------------------
// At 600 on a 1072 px panel the browser scales the whole page by about 1.79.
// Type survives that — it is rasterised at the final size, not upscaled — but
// a 1 px rule becomes 1.79 device px and lands soft across two rows of pixels.
// Laying out at the panel's own pixel count instead keeps hairlines on the
// grid.
//
// Whether that helps depends on what the reader's browser reports for its
// viewport and devicePixelRatio, which no amount of reasoning here can settle.
// GET /kindle/probe prints both off the device; pick the value from that.
//
//   600   default. Scaled up by the browser. Works on any firmware.
//   536   1072 / 2 — try this if the probe reports devicePixelRatio 2.
//   1072  the panel's own pixel count — try this if the probe reports 1.
//
// The vertical budget follows the same ratio: the page is laid out to fit
// PAGE_W * 1448 / 1072 tall, which is 810 at 600 and 1448 at 1072.
#ifndef KINDLE_PAGE_W
#  define KINDLE_PAGE_W 600
#endif
#if KINDLE_PAGE_W < 320 || KINDLE_PAGE_W > 2400
#  error "KINDLE_PAGE_W is outside the range this layout has been checked over"
#endif

// Rescales a number tuned at the 600 px layout onto KINDLE_PAGE_W. Sizes are
// written throughout as the figures the design was measured at and passed
// through here, so the source stays readable as the design and the build
// decides which pixel grid it lands on.
//
// Rounds half away from zero: several of these are negative (letter-spacing, a
// superscript's offset) and C's truncation would pull them toward zero and
// quietly loosen the tracking as the page grew.
//
// Outside the FEATURE_KINDLE_DASHBOARD guard on purpose — ForecastModule draws
// its condition glyphs through this and can be built with the dashboard off.
constexpr int kdPx(int n) {
    return (n >= 0) ? ( n * KINDLE_PAGE_W + 300) / 600
                    : -((-n * KINDLE_PAGE_W + 300) / 600);
}


#ifdef FEATURE_KINDLE_DASHBOARD

class AsyncWebServer;

// Which sensors the dashboard reads. These are sensor instance ids as
// configured in platform_config.json — "outdoor" is typically a remote node
// (see node/), "indoor" a locally wired BME280.
#ifndef KINDLE_OUTDOOR_SENSOR
#  define KINDLE_OUTDOOR_SENSOR "outdoor"
#endif
#ifndef KINDLE_INDOOR_SENSOR
#  define KINDLE_INDOOR_SENSOR  "indoor"
#endif

// ---------------------------------------------------------------------------
// When the panel repaints
// ---------------------------------------------------------------------------
// There are three ways this page gets redrawn, and it is worth being precise
// about which is which, because one of them is not what it sounds like.
//
// 1. The reader asks. A tap or a click on "refresh" in the footer. Works on a
//    touch reader and on a five-way pad alike, because it is a link.
//
// 2. The reader asks for a clean panel. "clear" walks /kindle/clear through a
//    few full-screen black and white frames and comes back. E-ink retains a
//    ghost of what it drew before; driving the whole panel to both extremes is
//    what clears it, and nothing a normal page draws does that.
//
// 3. The page reloads itself. This is a timer, not a push.
//
// THERE IS NO PUSH, AND WHY
// -------------------------
// The collector cannot make the reader repaint. A browser redraws when it
// loads a page, and it only loads a page when it asks for one. Server-sent
// events or a socket would need JavaScript the older firmware does not have,
// and holding a request open on AsyncTCP until data arrives risks the one
// thing this device must not do unattended.
//
// So KINDLE_FOLLOW_DATA predicts instead. The page knows when the newest
// reading landed and roughly how often readings come, so it sets its own
// reload to fire just after the next one is due. The panel then updates within
// a few seconds of new data without anything being pushed to it.
//
// THE COST, STATED PLAINLY
// ------------------------
// Every reload repaints the whole panel: it flashes, and it costs battery.
// Following a node that posts once a minute means flashing once a minute
// rather than once every five, which is a real trade and not a free win.
// KINDLE_REFRESH_MIN_SEC is the floor that keeps it from becoming a strobe.
// Set KINDLE_FOLLOW_DATA to 0 for the old fixed-interval behaviour.

// Ceiling: the longest the page will ever wait, and the fixed interval when
// KINDLE_FOLLOW_DATA is off.
#ifndef KINDLE_REFRESH_SEC
#  define KINDLE_REFRESH_SEC 300
#endif

// Floor: the shortest gap between two repaints, however fresh the data is.
#ifndef KINDLE_REFRESH_MIN_SEC
#  define KINDLE_REFRESH_MIN_SEC 60
#endif

// How often readings are expected, in seconds. The node's posting interval —
// 60 by default, see node/README.md. Only used to predict the next arrival.
#ifndef KINDLE_DATA_PERIOD_SEC
#  define KINDLE_DATA_PERIOD_SEC 60
#endif

// 1 = reload just after the next reading is due; 0 = fixed KINDLE_REFRESH_SEC.
#ifndef KINDLE_FOLLOW_DATA
#  define KINDLE_FOLLOW_DATA 1
#endif

// THE CLOCK IS THE OTHER HALF OF THIS, AND USUALLY THE LOUDER ONE
// ---------------------------------------------------------------
// The clock is rendered server-side. It is correct at the moment it is painted
// and stale from then on, so a clock showing minutes is a standing demand for
// a repaint every minute no matter what the data is doing.
//
// With this on, the reload is aimed at the next minute boundary, so the
// displayed minute changes when the minute changes rather than at some
// arbitrary offset, and it is never more than about a minute behind.
//
// At the default settings this always wins: the data floor is 60 s and the
// clock never asks for more than 60. KINDLE_FOLLOW_DATA therefore changes
// nothing unless this is off, or KINDLE_REFRESH_MIN_SEC drops below a minute
// with a node posting faster than that. Said out loud because it would
// otherwise look like the data logic is doing work it is not.
//
// WHAT IT COSTS: about 1440 page loads a day, each a full panel repaint. That
// is a reader on a charger, not one running on its battery for a fortnight.
// Turn this off and the clock goes stale by up to KINDLE_REFRESH_SEC between
// reloads — which for a 96 px clock on a shelf is a confident lie, so prefer
// dropping KINDLE_REFRESH_SEC to something the clock can live with instead.
#ifndef KINDLE_CLOCK_PIN_REFRESH
#  define KINDLE_CLOCK_PIN_REFRESH 1
#endif

#if KINDLE_REFRESH_MIN_SEC > KINDLE_REFRESH_SEC
#  error "KINDLE_REFRESH_MIN_SEC exceeds KINDLE_REFRESH_SEC: the floor is above the ceiling"
#endif

/// Registers the trend series this page draws. Call once from setup(),
/// before ProcessingTask starts, so no readings are missed.
void kindleTrackTrends();

/// Registers GET /kindle and GET /kindle/probe on `server`.
void registerKindleDashboard(AsyncWebServer& server);

#endif  // FEATURE_KINDLE_DASHBOARD
