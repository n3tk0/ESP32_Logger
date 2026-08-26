#include "KindleDashboard.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <math.h>

#include "../pipeline/DataPipeline.h"
#include "../pipeline/TrendRing.h"
#include "../utils/MutexGuard.h"
#include "../core/Globals.h"
#include "DashboardStrings.h"

#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif

static constexpr int PAGE_W  = KINDLE_PAGE_W;
static constexpr int CHART_W = kdPx(560);
static constexpr int CHART_H = kdPx(200);

// ---------------------------------------------------------------------------
// Reading the current values
// ---------------------------------------------------------------------------
struct Latest {
    float    value = NAN;
    uint32_t ts    = 0;
    bool     ok    = false;
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
    }
    return out;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
static void fmtTemp(char* buf, size_t n, float v) {
    if (!isfinite(v)) { snprintf(buf, n, "--"); return; }
    // One decimal. A tenth of a degree is the most an e-ink glance can use,
    // and two decimals make the big type wrap.
    snprintf(buf, n, "%.1f", (double)v);
}

static void fmtInt(char* buf, size_t n, float v) {
    if (!isfinite(v)) { snprintf(buf, n, "--"); return; }
    snprintf(buf, n, "%.0f", (double)v);
}


// A four-glyph reading needs a smaller face than a three-glyph one to sit in
// the same column, and there is no JavaScript here to measure it after the
// fact. Picking the class from the string length is the whole trick.
static const char* bigClass(const char* v) {
    return (strlen(v) >= 4) ? "big big4" : "big";
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

// ---------------------------------------------------------------------------
// How long until this page reloads itself
// ---------------------------------------------------------------------------
// A prediction, not a subscription — see the header. The page knows when the
// newest reading landed and how often readings are expected, so it aims its
// own reload just after the next one is due.
//
// The three cases below exist because "no reading yet" and "the node died"
// both look like "the data is old", and neither should make an e-ink panel
// flash every minute forever waiting for something that is not coming.
//
// AND THEN THERE IS THE CLOCK
// ---------------------------
// The clock is rendered server-side, so it is right at the moment it is
// painted and goes stale from there. A clock showing minutes is therefore a
// standing demand for a repaint every minute, and it does not care what the
// data is doing.
//
// So the delay is the smaller of the two demands, and the clock's is aimed at
// the next minute boundary rather than "60 seconds from now": that way the
// displayed minute changes when the minute actually changes, the way a clock
// is supposed to, instead of at some arbitrary offset.
//
// At the default settings the clock always wins — the data floor is 60 s and
// the clock never asks for more than 60 — so KINDLE_FOLLOW_DATA does nothing
// unless the clock is unpinned or KINDLE_REFRESH_MIN_SEC is dropped below a
// minute. That is stated rather than hidden: the shape of the data logic is
// still right, it is simply not the binding constraint while a minute clock
// is on the page.
#if KINDLE_CLOCK_PIN_REFRESH
static uint32_t clockDelaySec(uint32_t now) {
    uint32_t toBoundary = 60u - (now % 60u);     // 1..60
    // Landing within a couple of seconds of this render means flashing twice
    // in a breath, so give up on this boundary and take the next one. After
    // the first load the cadence settles onto a steady 60 s on the minute.
    if (toBoundary < 3u) toBoundary += 60u;
    return toBoundary;
}
#endif

static uint32_t refreshDelaySec(uint32_t newestTs, uint32_t now) {
#if KINDLE_CLOCK_PIN_REFRESH
    const uint32_t clockWants = clockDelaySec(now);
#else
    const uint32_t clockWants = 0xFFFFFFFFu;     // the clock makes no demand
#endif

    // Not a plain min(). Landing at :58 rather than :00 costs the same repaint
    // but shows the previous minute for 58 seconds of every minute, so a data
    // delay that merely happens to fall a second or two short of the boundary
    // must not steal the alignment. It has to be meaningfully earlier — five
    // seconds — before it is worth breaking the clock's cadence for.
    #define KD_MIN_CLOCK(d) (((d) + 5u < clockWants) ? (uint32_t)(d) : clockWants)
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
            d = period - age + 4;
        } else if (age < period * 2) {
            // Late, but one missed post is ordinary. Look again soon.
            d = KINDLE_REFRESH_MIN_SEC;
        } else {
            // Two periods with nothing is a source that is down. Back off to
            // the ceiling; the page already says the reading is stale, and
            // flashing at it will not bring the node back.
            return KD_MIN_CLOCK((uint32_t)KINDLE_REFRESH_SEC);
        }

        if (d < KINDLE_REFRESH_MIN_SEC) d = KINDLE_REFRESH_MIN_SEC;
        if (d > KINDLE_REFRESH_SEC)     d = KINDLE_REFRESH_SEC;
        return KD_MIN_CLOCK(d);
    }
#else
    (void)newestTs;
#endif
    return KD_MIN_CLOCK((uint32_t)KINDLE_REFRESH_SEC);
    #undef KD_MIN_CLOCK
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
static void handleKindle(AsyncWebServerRequest* req) {
    const Latest outT = latestOf(KINDLE_OUTDOOR_SENSOR, "temperature");
    const Latest outH = latestOf(KINDLE_OUTDOOR_SENSOR, "humidity");
    const Latest outP = latestOf(KINDLE_OUTDOOR_SENSOR, "pressure");
    const Latest inT  = latestOf(KINDLE_INDOOR_SENSOR,  "temperature");
    const Latest inH  = latestOf(KINDLE_INDOOR_SENSOR,  "humidity");

    const uint32_t now = (uint32_t)time(nullptr);

    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn [TrendRing::HOURS];
    TrendRing::Hour tPress[TrendRing::HOURS];
    const bool haveOut = trendRing.series(KINDLE_OUTDOOR_SENSOR, "temperature", now, tOut);
    const bool haveIn  = trendRing.series(KINDLE_INDOOR_SENSOR,  "temperature", now, tIn);
    const bool haveP   = trendRing.series(KINDLE_OUTDOOR_SENSOR, "pressure",    now, tPress);

    String p;
    p.reserve(7000);
    char buf[16];

    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
    p += F("\"><meta http-equiv=\"refresh\" content=\"");
    // Newest of the two sensors: the page is current if either one is, and
    // waiting on the slower of the pair would show a stale outdoor reading.
    p += refreshDelaySec(outT.ts > inT.ts ? outT.ts : inT.ts, now);
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

    KD_S(".lab{font-size:");                   KD_N(12);
    KD_S("px;letter-spacing:");                KD_N(4);
    KD_S("px;text-transform:uppercase;margin-bottom:"); KD_N(2);
    KD_S("px;color:#777}");

    // Fixed height, not line-height alone: the two columns use different
    // sizes, and without it the text under the smaller numeral starts higher
    // and the column rule looks ragged.
    KD_S(".big{font-size:");                   KD_N(92);
    KD_S("px;line-height:");                   KD_N(88);
    KD_S("px;height:");                        KD_N(88);
    KD_S("px;letter-spacing:");                KD_N(-3);
    KD_S("px}");

    // Four glyphs ("21.0", "-8.4") overrun the column at the larger size.
    KD_S(".big4{font-size:");                  KD_N(74);
    KD_S("px;letter-spacing:");                KD_N(-2);
    KD_S("px}");

    KD_S(".deg{font-size:");                   KD_N(30);
    KD_S("px;letter-spacing:0;vertical-align:top;line-height:1;"
         "position:relative;top:");            KD_N(12);
    KD_S("px}");

    KD_S(".sub{font-size:");                   KD_N(15);
    KD_S("px;margin-top:");                    KD_N(6);
    KD_S("px;line-height:1.45;color:#444}");
    KD_S(".sub-t{margin-top:");                KD_N(1);
    KD_S("px}");
    KD_S(".dim{color:#777}");

    // Temperature and humidity are one reading of one parcel of air, so they
    // share a baseline and a slash rather than a line break.
    KD_S(".slash{font-size:");                 KD_N(44);
    KD_S("px;color:#aaa;letter-spacing:0;padding:0 "); KD_N(7);
    KD_S("px;position:relative;top:");         KD_N(-5);
    KD_S("px}");
    KD_S(".hum-o{font-size:");                 KD_N(44);
    KD_S("px;color:#444;letter-spacing:");     KD_N(-1);
    KD_S("px}");

    // A six-glyph reading ("-12.4") already drops to .big4; the pair beside it
    // has to come down too or "100%" runs off the column.
    KD_S(".big4 .slash{font-size:");           KD_N(34);
    KD_S("px;padding:0 ");                     KD_N(5);
    KD_S("px;top:");                           KD_N(-4);
    KD_S("px}");
    KD_S(".big4 .hum-o{font-size:");           KD_N(34);
    KD_S("px}");

    // The absolute pressure is the one figure here compared against memory
    // rather than against the page; at 15px it was set as a footnote to the
    // humidity.
    KD_S(".pres{font-size:");                  KD_N(34);
    KD_S("px;line-height:1.15;margin-top:");   KD_N(5);
    KD_S("px;letter-spacing:");                KD_N(-1);
    KD_S("px}");
    KD_S(".pres-u{font-size:");                KD_N(17);
    KD_S("px;color:#777;letter-spacing:0}");

    // Right column, upper two thirds. Fixed height rather than line-height
    // alone, for the same reason .big has one: it is what keeps the divider
    // below it level with the left column's text. The height is set so the
    // divider lands two thirds of the way down the cell the left column sizes
    // — at 116 the inside block finished short and left a hole under it.
    KD_S(".clock{font-size:");                 KD_N(96);
    KD_S("px;line-height:");                   KD_N(104);
    KD_S("px;height:");                        KD_N(139);
    KD_S("px;padding-top:");                   KD_N(12);
    KD_S("px;letter-spacing:");                KD_N(-4);
    KD_S("px}");
    KD_S(".clock-x{font-size:");               KD_N(44);
    KD_S("px;line-height:");                   KD_N(104);
    KD_S("px;height:");                        KD_N(116);
    KD_S("px;color:#777;letter-spacing:0}");

    KD_S(".inrule{border-top:");               KD_N(1);
    KD_S("px solid #aaa;margin-bottom:");      KD_N(7);
    KD_S("px}");
    KD_S(".in-t{font-size:");                  KD_N(40);
    KD_S("px;line-height:1.05;letter-spacing:"); KD_N(-1);
    KD_S("px}");
    KD_S(".in-d{font-size:");                  KD_N(19);
    KD_S("px;vertical-align:top;letter-spacing:0;position:relative;top:"); KD_N(5);
    KD_S("px}");
    KD_S(".slash-i{font-size:");               KD_N(26);
    KD_S("px;padding:0 ");                     KD_N(5);
    KD_S("px;top:");                           KD_N(-2);
    KD_S("px}");
    KD_S(".hum-i{font-size:");                 KD_N(30);
    KD_S("px;color:#444}");
    KD_S(".in-age{font-size:");                KD_N(13);
    KD_S("px;letter-spacing:0;margin-left:");  KD_N(10);
    KD_S("px}");

    // Three rules on the page, so a few px each is what keeps the footer above
    // the fold. Measured, not guessed.
    KD_S(".rule{border-top:");                 KD_N(1);
    KD_S("px solid #aaa;margin:");             KD_N(14);
    KD_S("px 0 ");                             KD_N(9);
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

    // 44 device px is the smallest thing worth aiming at with a fingertip;
    // at this layout that is kdPx(24) of height, which is what the padding
    // buys. Boxed rather than underlined so the target is visible before it
    // is touched, which on a panel with no hover state is the only chance.
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

    p += F("</style></head><body>");

    // No masthead. The place name never changed and the date is carried by the
    // week strip at the foot, so the row was two lines of furniture above the
    // only two numbers the page exists to show. The hero row is the masthead.

    // Outside on the left, clock over inside on the right
    p += F("<table class=\"hero\"><tr><td width=\"50%\">"
           "<div class=\"lab\">" KD_T("Outside", "Навън") "</div><div class=\"");
    fmtTemp(buf, sizeof(buf), outT.value);
    p += bigClass(buf); p += F("\">"); p += buf;
    p += F("<span class=\"deg\">&deg;</span>");
    // Humidity rides on the temperature's baseline, a slash between them. It
    // is the same measurement of the same air at the same instant, so a line
    // break was putting a paragraph boundary through one reading.
    if (outH.ok) {
        p += F("<span class=\"slash\">/</span><span class=\"hum-o\">");
        fmtInt(buf, sizeof(buf), outH.value); p += buf; p += F("%</span>");
    }
    p += F("</div><div class=\"sub\">");
    if (outT.ok) {
        float mn, mx;
        if (haveOut && windowExtremes(tOut, mn, mx)) {
            // The 24 h span in figures, so the night's low is not only
            // readable off the chart.
            fmtTemp(buf, sizeof(buf), mn); p += buf;
            p += F(KD_T(" to ", " до ")); fmtTemp(buf, sizeof(buf), mx); p += buf;
            p += F("&deg;");
        }
        p += F("<span class=\"dim\">");
        appendAge(p, outT.ts, now);
        p += F("</span>");
    } else {
        p += F(KD_T("<span class=\"dim\">no reading &mdash; check the node</span>",
                    "<span class=\"dim\">няма данни &mdash; провери възела</span>"));
    }
    p += F("</div>");

    // Pressure gets its own size. The absolute figure is the one number here
    // that a reader compares against memory rather than against the page, and
    // at 15px it was set as a footnote to the humidity.
    if (outP.ok) {
        p += F("<div class=\"pres\">");
        fmtInt(buf, sizeof(buf), outP.value); p += buf;
        p += F("<span class=\"pres-u\"> hPa</span></div>");
    }
    if (haveP) {
        const Tendency t = pressureTendency(tPress);
        if (t.have) {
            p += F("<div class=\"sub sub-t\">"); p += t.arrow; p += F(" "); p += t.word;
            p += F(" <span class=\"dim\">(");
            if (t.delta >= 0) p += F("+");
            p += String(t.delta, 1); p += F(" hPa/3h)</span></div>");
        }
    }
    p += F("</td><td width=\"50%\" class=\"sep\">");

    // Upper two thirds: the time. An e-ink panel on a shelf is read across a
    // room, and until now the only clock was 14px of grey at the top corner.
    if (now > 1000000000u) {
        const time_t when_t = (time_t)now;
        struct tm tmv;
        if (localtime_r(&when_t, &tmv) != nullptr) {
            char hm[8];
            snprintf(hm, sizeof(hm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            p += F("<div class=\"clock\">"); p += hm; p += F("</div>");
        } else {
            p += F("<div class=\"clock-x\">" KD_T("no time", "няма час") "</div>");
        }
    } else {
        // Not "--:--": a plausible-looking blank clock invites the reader to
        // wonder what time it is, where "clock not set" names the fault.
        p += F("<div class=\"clock-x\">" KD_T("clock not set", "часът не е сверен") "</div>");
    }

    // Lower third: inside temperature and humidity. The 24 h inside range went
    // with the clock — the dashed line on the chart already carries it, and it
    // was the least-read figure on the page.
    p += F("<div class=\"inrule\"></div>"
           "<div class=\"lab\">" KD_T("Inside", "Вътре") "</div><div class=\"in-t\">");
    if (inT.ok) {
        fmtTemp(buf, sizeof(buf), inT.value); p += buf;
        p += F("<span class=\"in-d\">&deg;</span>");
        if (inH.ok) {
            p += F("<span class=\"slash slash-i\">/</span><span class=\"hum-i\">");
            fmtInt(buf, sizeof(buf), inH.value); p += buf; p += F("%</span>");
        }
        p += F("<span class=\"in-age dim\">");
        appendAge(p, inT.ts, now);
        p += F("</span>");
    } else {
        p += F("<span class=\"in-age dim\">"
               KD_T("no reading", "няма данни") "</span>");
    }
    p += F("</div></td></tr></table>");

    p += F("<div class=\"rule\"></div><div class=\"sec\">" KD_T("Last 24 hours", "Последните 24 часа") "</div>");
    appendChart(p, tOut, tIn, haveOut, haveIn);
    if (haveOut || haveIn) {
        // The two swatches must be drawn with the same stroke as the lines they
        // stand for — .l-out #000/3, .l-in #777/2 dashed — or the key describes
        // a chart the reader is not looking at.
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

#ifdef MODULE_FORECAST_ENABLED
    // Last, deliberately: the measured values are what the reader came for and
    // the forecast is the supporting note, so it reads as a footnote rather
    // than as something competing with the two temperatures.
    appendForecastSection(p);
#endif

    appendWeek(p, now);

    // The footer carries the two manual repaints. Links rather than anything
    // scripted, so a five-way pad reaches them as readily as a fingertip, and
    // padded to a real target: 44 device px is the smallest thing worth aiming
    // at on a touch panel, which is kdPx(24) at this layout.
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
    trendRing.track(KINDLE_OUTDOOR_SENSOR, "temperature");
    trendRing.track(KINDLE_INDOOR_SENSOR,  "temperature");
    trendRing.track(KINDLE_OUTDOOR_SENSOR, "pressure");
    trendRing.track(KINDLE_OUTDOOR_SENSOR, "humidity");
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

void registerKindleDashboard(AsyncWebServer& server) {
    server.on("/kindle", HTTP_GET, handleKindle);
    server.on("/kindle/probe", HTTP_GET, handleKindleProbe);
    server.on("/kindle/clear", HTTP_GET, handleKindleClear);
}

#endif  // FEATURE_KINDLE_DASHBOARD
