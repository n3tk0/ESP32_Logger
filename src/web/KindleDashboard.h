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

// Seconds between automatic page reloads.
#ifndef KINDLE_REFRESH_SEC
#  define KINDLE_REFRESH_SEC 300
#endif

/// Registers the trend series this page draws. Call once from setup(),
/// before ProcessingTask starts, so no readings are missed.
void kindleTrackTrends();

/// Registers GET /kindle and GET /kindle/probe on `server`.
void registerKindleDashboard(AsyncWebServer& server);

#endif  // FEATURE_KINDLE_DASHBOARD
