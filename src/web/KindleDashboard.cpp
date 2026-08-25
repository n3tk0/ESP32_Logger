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

// Layout constants, in CSS pixels. 600 is the width this page pins with its
// own viewport meta, not a width the device reports — see KindleDashboard.h.
// Everything is sized off it rather than in percentages, because percentage
// widths inside tables behave inconsistently on that browser.
static constexpr int PAGE_W  = 600;
static constexpr int CHART_W = 560;
static constexpr int CHART_H = 200;

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

    const int L = 40, R = CHART_W - 4, T = 10, B = CHART_H - 26;
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
        out += F("<text class=\"ax\" x=\""); out += L - 7;
        out += F("\" y=\""); out += y + 4;
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
        out += F("\" y=\""); out += CHART_H - 8;
        out += F("\" text-anchor=\"middle\">-");
        out += (TrendRing::HOURS - 1 - i);
        out += F("h</text>");
    }
    // The right-hand edge is now, and the stride above never lands on it.
    // Leaving it bare made the axis read as if it stopped five hours ago.
    out += F("<text class=\"ax\" x=\""); out += KD_X(TrendRing::HOURS - 1);
    out += F("\" y=\""); out += CHART_H - 8;
    out += F("\" text-anchor=\"end\">" KD_T("now", "сега") "</text>");

    #undef KD_X
    #undef KD_Y
    out += F("</svg>");
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
    p += KINDLE_REFRESH_SEC;
    p += F("\"><title>" KD_T("Weather", "Времето") "</title><style>"
           "body{font-family:Bookerly,Caecilia,Georgia,'Times New Roman',serif;"
           "margin:0;padding:20px 18px;background:#fff;color:#000;"
           "-webkit-text-size-adjust:none}"
           "*{box-sizing:border-box}"
           "table{width:100%;border-collapse:collapse}"
           "td{vertical-align:top;padding:0}"
           /* Palette: #000 #444 #777 #aaa #d8d8d8 #fff. The panel has 16 real
              grey levels — the dithering that argued against greys here comes
              from gradients and from tones too close together, not from flat
              well-separated fills. Spaced this far apart each renders solid. */
           ".hero td{padding:2px 0 6px}"
           /* Two classes, not one: .hero td above is (0,1,1) and would otherwise
              outrank a bare .sep (0,1,0), zeroing this padding and letting the
              rule sit against the first glyph of the inside reading. */
           ".hero .sep{border-left:1px solid #aaa;padding-left:30px}"
           ".lab{font-size:12px;letter-spacing:4px;text-transform:uppercase;"
           "margin-bottom:2px;color:#777}"
           ".big{font-size:92px;line-height:88px;height:88px;letter-spacing:-3px}"
           /* Fixed height, not line-height alone: the two columns use
              different sizes, and without it the text under the smaller
              numeral starts higher and the column rule looks ragged. */
           /* Four glyphs ("21.0", "-8.4") overrun a 280px column at 92px. */
           ".big4{font-size:74px;letter-spacing:-2px}"
           ".deg{font-size:30px;letter-spacing:0;vertical-align:top;"
           "line-height:1;position:relative;top:12px}"
           ".sub{font-size:15px;margin-top:6px;line-height:1.45;color:#444}"
           ".sub-t{margin-top:1px}"
           ".dim{color:#777}"
           /* Temperature and humidity are one reading of one parcel of air,
              so they share a baseline and a slash rather than a line break. */
           ".slash{font-size:44px;color:#aaa;letter-spacing:0;padding:0 7px;"
           "position:relative;top:-5px}"
           ".hum-o{font-size:44px;color:#444;letter-spacing:-1px}"
           /* A six-glyph reading ("-12.4") already drops to .big4; the pair
              beside it has to come down too or "100%" runs off the column. */
           ".big4 .slash{font-size:34px;padding:0 5px;top:-4px}"
           ".big4 .hum-o{font-size:34px}"
           /* The absolute pressure is the one figure here compared against
              memory rather than against the page; at 15px it was set as a
              footnote to the humidity. */
           ".pres{font-size:34px;line-height:1.15;margin-top:5px;"
           "letter-spacing:-1px}"
           ".pres-u{font-size:17px;color:#777;letter-spacing:0}"
           /* Right column, upper two thirds. Fixed height rather than
              line-height alone, for the same reason .big has one: it is what
              keeps the divider below it level with the left column's text. */
           /* Height set so the divider lands two thirds of the way down the cell
              the left column sizes: at 116px the inside block finished 24px
              short of the bottom and left a hole under it. */
           ".clock{font-size:96px;line-height:104px;height:139px;padding-top:12px;"
           "letter-spacing:-4px}"
           ".clock-x{font-size:44px;line-height:104px;height:116px;color:#777;"
           "letter-spacing:0}"
           ".inrule{border-top:1px solid #aaa;margin-bottom:7px}"
           ".in-t{font-size:40px;line-height:1.05;letter-spacing:-1px}"
           ".in-d{font-size:19px;vertical-align:top;letter-spacing:0;"
           "position:relative;top:5px}"
           ".slash-i{font-size:26px;padding:0 5px;top:-2px}"
           ".hum-i{font-size:30px;color:#444}"
           ".in-age{font-size:13px;letter-spacing:0;margin-left:10px}"
           /* Three rules on the page, so a few px each is what keeps the
              footer above the 800px fold. Measured, not guessed. */
           ".rule{border-top:1px solid #aaa;margin:14px 0 10px}"
           ".sec{font-size:12px;letter-spacing:4px;text-transform:uppercase;"
           "margin-bottom:8px;color:#777}"
           ".ico{vertical-align:top;padding-top:4px}"
           ".fc{font-size:28px;line-height:1.1;padding-left:12px}"
           ".fc-t{font-size:30px;margin-top:1px;color:#000}"
           /* Equal thirds of the right half; nowrap so a two-part daily
              figure never breaks across lines. */
           ".per{width:88px;text-align:center;vertical-align:top;white-space:nowrap;"
           "background:#f0f0f0;border-left:4px solid #fff}"
           ".per-l{font-size:11px;letter-spacing:2px;text-transform:uppercase;"
           "margin-bottom:1px;color:#777;padding-top:4px}"
           ".per-t{font-size:19px;margin-top:-2px;padding-bottom:5px}"
           ".chart{display:block;margin:2px auto 0}"
           ".grid{stroke:#c4c4c4;stroke-width:1}"
           ".vgrid{stroke:#d5d5d5;stroke-width:1}"
           ".base{stroke:#777;stroke-width:1}"
           ".ax{font-size:11px;fill:#777;font-family:Bookerly,Georgia,serif}"
           ".band{fill:#d8d8d8;stroke:#8f8f8f;stroke-width:1}"
           ".l-out{fill:none;stroke:#000;stroke-width:3}"
           ".l-in{fill:none;stroke:#777;stroke-width:2;stroke-dasharray:7 5}"
           ".key{font-size:13px;margin-top:2px;color:#444}"
           ".key td{padding-top:2px}"
           ".note{font-size:15px;font-style:italic;text-align:center;"
           "padding:36px 0;color:#777}"
           /* Tighter under its heading than the two sections above: the
              strip is a row of blocks, not a paragraph, and the extra gap
              was what pushed the footer below the fold. */
           ".sec-wk{margin-bottom:3px}"
           ".wk{margin-top:2px}"
           ".wd{width:14.28%;text-align:center;padding:8px 0 7px;background:#f4f4f4}"
           ".wd-we{background:#e4e4e4}"
           ".wd-n{font-size:11px;letter-spacing:2px;text-transform:uppercase;color:#777}"
           ".wd-d{font-size:24px;line-height:1.15}"
           /* Inverted rather than outlined: a filled block is the one
              mark that stays unambiguous after e-ink dithering, where a
              thin ring can read as a smudge. */
           ".wd-now{background:#000;color:#fff}"
           ".foot{border-top:1px solid #aaa;margin-top:11px;padding-top:5px;"
           "font-size:12px;color:#555;letter-spacing:.5px}"
           "</style></head><body>");

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
        p += F("<table class=\"key\"><tr><td>"
               "<svg width=\"26\" height=\"9\"><line x1=\"0\" y1=\"5\" x2=\"26\" "
               "y2=\"5\" stroke=\"#000\" stroke-width=\"3\"/></svg> "
               KD_T("outside mean", "средно навън")
               "<span class=\"dim\">"
               KD_T(", shaded band = hourly low to high",
                    ", сивото е час. мин&ndash;макс")
               "</span>"
               "</td><td style=\"text-align:right\">"
               "<svg width=\"26\" height=\"9\"><line x1=\"0\" y1=\"5\" x2=\"26\" "
               "y2=\"5\" stroke=\"#777\" stroke-width=\"2\" "
               "stroke-dasharray=\"7 5\"/></svg> " KD_T("inside", "вътре")
               "</td></tr></table>");
    }

#ifdef MODULE_FORECAST_ENABLED
    // Last, deliberately: the measured values are what the reader came for and
    // the forecast is the supporting note, so it reads as a footnote rather
    // than as something competing with the two temperatures.
    appendForecastSection(p);
#endif

    appendWeek(p, now);

    p += F(KD_T("<div class=\"foot\">Measured on site &middot; refreshes every ",
                "<div class=\"foot\">Измерено на място &middot; обновява се на "));
    p += (KINDLE_REFRESH_SEC / 60);
    p += F(KD_T(" minutes</div></body></html>", " минути</div></body></html>"));

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

void registerKindleDashboard(AsyncWebServer& server) {
    server.on("/kindle", HTTP_GET, handleKindle);
}

#endif  // FEATURE_KINDLE_DASHBOARD
