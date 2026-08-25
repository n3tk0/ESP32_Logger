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
// <meta name="viewport" content="width=600">, so the browser scales 600 CSS px
// across the panel's 1072 device px — about 1.79× — and the 1448 px of height
// then works out to roughly 810 CSS px at that same scale. That is the whole
// derivation, and it is why the page is measured against an 800 px budget: it
// holds for any 1072×1448 reader regardless of what devicePixelRatio says.
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

/// Registers GET /kindle on `server`.
void registerKindleDashboard(AsyncWebServer& server);

#endif  // FEATURE_KINDLE_DASHBOARD
