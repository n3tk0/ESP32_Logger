// ============================================================================
// src/web/KindleDashboard.h
//
// GET /kindle — a dashboard for a 6" e-ink reader's browser.
//
// TARGET
// ------
// Kindle Paperwhite 3 (model PQ94WIF, 2015) and anything newer. The panel is
// 1072×1448 at 300 ppi, but the browser reports roughly 600×800 CSS pixels at
// devicePixelRatio 2, and 600×800 is what this page is laid out for. Newer
// readers have more room and simply get more margin.
//
// WHAT THAT BROWSER CANNOT DO
// ---------------------------
// It is a WebKit build from around 2012. No fetch, no Promise, no ES6, no
// flexbox, no CSS grid, no web fonts worth the bytes. So:
//
//   • the page is rendered server-side and ships zero JavaScript;
//   • layout is tables and blocks, because those are what actually work;
//   • the trend chart is inline SVG, drawn here as path data — no canvas,
//     no charting library, nothing to execute;
//   • it refreshes with <meta http-equiv="refresh">, not a timer.
//
// WHY GRAYSCALE IS NOT JUST A PALETTE CHOICE
// ------------------------------------------
// The panel is 16-level grayscale, and mid greys dither into visible noise at
// this size. Everything here is black, white, or one of two greys chosen to
// stay distinguishable after dithering. Two lines on one chart are told apart
// by dash pattern, not by shade, because shade does not survive.
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
