#include "KindleDashboard.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <math.h>

#include "../pipeline/DataPipeline.h"
#include "../pipeline/TrendRing.h"
#include "../utils/MutexGuard.h"
#include "../core/Globals.h"

#ifdef MODULE_FORECAST_ENABLED
#  include "../modules/ForecastModule.h"
#endif

// Layout constants, in CSS pixels. 600 wide is the Paperwhite 3's usable
// viewport; everything is sized off that rather than percentages, because
// percentage widths inside tables behave inconsistently on that browser.
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
    if      (t.delta >=  1.6f) { t.word = "rising fast";  t.arrow = "&#8593;"; }
    else if (t.delta >=  0.5f) { t.word = "rising";       t.arrow = "&#8599;"; }
    else if (t.delta >  -0.5f) { t.word = "steady";       t.arrow = "&#8594;"; }
    else if (t.delta >  -1.6f) { t.word = "falling";      t.arrow = "&#8600;"; }
    else                       { t.word = "falling fast"; t.arrow = "&#8595;"; }
    return t;
}

// ---------------------------------------------------------------------------
// The trend chart
// ---------------------------------------------------------------------------
// Two series over 24 hours. The outdoor series is drawn as a hatched BAND
// between its hourly min and max with the mean as a solid line through it;
// indoor is a dashed mean line only.
//
// The band is the point. TrendRing keeps min/max per hour precisely so an
// overnight excursion is visible, and a mean-only line throws that away — a
// night that dipped to -3 and recovered by dawn looks identical to one that
// sat at +2. It also costs nothing: the data was already being stored.
//
// Hatching rather than a flat grey fill: 16-level e-ink dithers mid-greys
// into visible noise at this size, while a hard-edged 1px diagonal rule
// renders cleanly and reads as "range" the way a printed chart does.
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
        out += F("<p class=\"note\">The 24 hour record fills as readings arrive.</p>");
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
    out += F("\"><defs><pattern id=\"h\" width=\"4\" height=\"4\" "
             "patternUnits=\"userSpaceOnUse\" patternTransform=\"rotate(45)\">"
             "<line x1=\"0\" y1=\"0\" x2=\"0\" y2=\"4\" stroke=\"#000\" "
             "stroke-width=\"1\"/></pattern></defs>");

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
    out += F("\" text-anchor=\"end\">now</text>");

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
    if (mins < 60) { out += mins; out += F(" min old"); }
    else           { out += (mins / 60); out += F(" h old"); }
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

    // Walk back to Monday in whole days. Doing it on the epoch rather than on
    // tm_mday keeps month and year ends correct for free.
    static const char* NAMES[7] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };
    out += F("<div class=\"rule\"></div><table class=\"wk\"><tr>");
    for (int i = 0; i < 7; i++) {
        const time_t day = t + (time_t)(i - todayIdx) * 86400;
        struct tm dv;
        if (localtime_r(&day, &dv) == nullptr) continue;
        out += F("<td class=\"");
        out += (i == todayIdx) ? F("wd wd-now") : F("wd");
        out += F("\"><div class=\"wd-n\">");
        out += NAMES[i];
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
// paper in every way that matters: reflective, static, monochrome, redrawn in
// full or not at all. So — rules instead of boxes, a masthead instead of a
// header bar, letterspaced small caps instead of chips, and the reader's own
// serif faces (Bookerly and Caecilia ship on the device) instead of a webfont
// that would cost bytes to render worse.
//
// Nothing here uses flexbox, grid, CSS variables or calc(): the Paperwhite 3's
// browser is a 2012 WebKit and supports none of them. Tables and blocks with
// literal pixel values are what survives.
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
    p += F("\"><title>Weather</title><style>"
           "body{font-family:Bookerly,Caecilia,Georgia,'Times New Roman',serif;"
           "margin:0;padding:20px 18px;background:#fff;color:#000;"
           "-webkit-text-size-adjust:none}"
           "*{box-sizing:border-box}"
           "table{width:100%;border-collapse:collapse}"
           "td{vertical-align:top;padding:0}"
           ".mast{border-bottom:3px solid #000;padding-bottom:5px;margin-bottom:2px}"
           ".mast-sub{border-bottom:1px solid #000;height:3px;margin-bottom:16px}"
           ".place{font-size:15px;letter-spacing:5px;text-transform:uppercase}"
           ".when{font-size:14px;text-align:right;letter-spacing:1px}"
           ".hero td{padding:2px 0 6px}"
           /* Two classes, not one: .hero td above is (0,1,1) and would otherwise
              outrank a bare .sep (0,1,0), zeroing this padding and letting the
              rule sit against the first glyph of the inside reading. */
           ".hero .sep{border-left:1px solid #000;padding-left:30px}"
           ".lab{font-size:12px;letter-spacing:4px;text-transform:uppercase;"
           "margin-bottom:2px}"
           ".big{font-size:92px;line-height:88px;height:88px;letter-spacing:-3px}"
           /* Fixed height, not line-height alone: the two columns use
              different sizes, and without it the text under the smaller
              numeral starts higher and the column rule looks ragged. */
           /* Four glyphs ("21.0", "-8.4") overrun a 280px column at 92px. */
           ".big4{font-size:74px;letter-spacing:-2px}"
           ".deg{font-size:30px;letter-spacing:0;vertical-align:top;"
           "line-height:1;position:relative;top:12px}"
           ".sub{font-size:15px;margin-top:6px;line-height:1.45}"
           ".dim{color:#555}"
           /* Three rules on the page, so a few px each is what keeps the
              footer above the 800px fold. Measured, not guessed. */
           ".rule{border-top:1px solid #000;margin:13px 0 10px}"
           ".sec{font-size:12px;letter-spacing:4px;text-transform:uppercase;"
           "margin-bottom:8px}"
           ".ico{vertical-align:top;padding-top:4px}"
           ".fc{font-size:28px;line-height:1.1;padding-left:12px}"
           ".fc-t{font-size:30px;margin-top:1px}"
           /* Equal thirds of the right half; nowrap so a two-part daily
              figure never breaks across lines. */
           ".per{width:88px;text-align:center;vertical-align:top;white-space:nowrap}"
           ".per-l{font-size:11px;letter-spacing:2px;text-transform:uppercase;margin-bottom:1px}"
           ".per-t{font-size:19px;margin-top:-2px}"
           ".chart{display:block;margin:2px auto 0}"
           ".grid{stroke:#999;stroke-width:1}"
           ".base{stroke:#000;stroke-width:1}"
           ".ax{font-size:11px;fill:#000;font-family:Bookerly,Georgia,serif}"
           ".band{fill:url(#h);stroke:#000;stroke-width:1}"
           ".l-out{fill:none;stroke:#000;stroke-width:3}"
           ".l-in{fill:none;stroke:#000;stroke-width:2;stroke-dasharray:7 5}"
           ".key{font-size:13px;margin-top:2px}"
           ".key td{padding-top:2px}"
           ".note{font-size:15px;font-style:italic;text-align:center;"
           "padding:36px 0;color:#555}"
           ".wk{margin-top:2px}"
           ".wd{width:14.28%;text-align:center;padding:5px 0 4px}"
           ".wd-n{font-size:11px;letter-spacing:2px;text-transform:uppercase}"
           ".wd-d{font-size:24px;line-height:1.15}"
           /* Inverted rather than outlined: a filled block is the one
              mark that stays unambiguous after e-ink dithering, where a
              thin ring can read as a smudge. */
           ".wd-now{background:#000;color:#fff}"
           ".foot{border-top:1px solid #000;margin-top:11px;padding-top:5px;"
           "font-size:12px;color:#555;letter-spacing:.5px}"
           "</style></head><body>");

    // Masthead
    p += F("<div class=\"mast\"><table><tr><td class=\"place\">");
    p += (config.deviceName[0] ? config.deviceName : "Weather");
    p += F("</td><td class=\"when\">");
    if (now > 1000000000u) {
        const time_t when_t = (time_t)now;
        struct tm tmv;
        char when[48];
        if (localtime_r(&when_t, &tmv) != nullptr &&
            strftime(when, sizeof(when), "%A %e %B &middot; %H:%M", &tmv) > 0) {
            p += when;
        } else {
            p += F("time unavailable");
        }
    } else {
        p += F("clock not set");
    }
    p += F("</td></tr></table></div><div class=\"mast-sub\"></div>");

    // The two temperatures
    p += F("<table class=\"hero\"><tr><td width=\"50%\">"
           "<div class=\"lab\">Outside</div><div class=\"");
    fmtTemp(buf, sizeof(buf), outT.value);
    p += bigClass(buf); p += F("\">"); p += buf;
    p += F("<span class=\"deg\">&deg;</span></div><div class=\"sub\">");
    if (outT.ok) {
        float mn, mx;
        if (haveOut && windowExtremes(tOut, mn, mx)) {
            // The 24 h span in words, so the night's low is not only readable
            // off the chart.
            fmtTemp(buf, sizeof(buf), mn); p += buf;
            p += F(" to "); fmtTemp(buf, sizeof(buf), mx); p += buf;
            p += F("&deg; today<br>");
        }
        if (outH.ok) { fmtInt(buf, sizeof(buf), outH.value); p += buf; p += F("% humidity"); }
        if (outH.ok && outP.ok) p += F(" &middot; ");
        if (outP.ok) { fmtInt(buf, sizeof(buf), outP.value); p += buf; p += F(" hPa"); }
        if (haveP) {
            const Tendency t = pressureTendency(tPress);
            if (t.have) {
                p += F("<br>"); p += t.arrow; p += F(" "); p += t.word;
                p += F(" <span class=\"dim\">(");
                if (t.delta >= 0) p += F("+");
                p += String(t.delta, 1); p += F(" hPa/3h)</span>");
            }
        }
        p += F("<span class=\"dim\">");
        appendAge(p, outT.ts, now);
        p += F("</span>");
    } else {
        p += F("<span class=\"dim\">no reading &mdash; check the node</span>");
    }
    p += F("</div></td><td width=\"50%\" class=\"sep\">"
           "<div class=\"lab\">Inside</div><div class=\"");
    fmtTemp(buf, sizeof(buf), inT.value);
    p += bigClass(buf); p += F("\">"); p += buf;
    p += F("<span class=\"deg\">&deg;</span></div><div class=\"sub\">");
    if (inT.ok) {
        float mn, mx;
        if (haveIn && windowExtremes(tIn, mn, mx)) {
            fmtTemp(buf, sizeof(buf), mn); p += buf;
            p += F(" to "); fmtTemp(buf, sizeof(buf), mx); p += buf;
            p += F("&deg; today<br>");
        }
        if (inH.ok) { fmtInt(buf, sizeof(buf), inH.value); p += buf; p += F("% humidity"); }
        p += F("<span class=\"dim\">");
        appendAge(p, inT.ts, now);
        p += F("</span>");
    } else {
        p += F("<span class=\"dim\">no reading</span>");
    }
    p += F("</div></td></tr></table>");

    p += F("<div class=\"rule\"></div><div class=\"sec\">Last 24 hours</div>");
    appendChart(p, tOut, tIn, haveOut, haveIn);
    if (haveOut || haveIn) {
        p += F("<table class=\"key\"><tr><td>"
               "<svg width=\"26\" height=\"9\"><line x1=\"0\" y1=\"5\" x2=\"26\" "
               "y2=\"5\" stroke=\"#000\" stroke-width=\"3\"/></svg> outside mean"
               "<span class=\"dim\">, hatched band = hourly low to high</span>"
               "</td><td style=\"text-align:right\">"
               "<svg width=\"26\" height=\"9\"><line x1=\"0\" y1=\"5\" x2=\"26\" "
               "y2=\"5\" stroke=\"#000\" stroke-width=\"2\" "
               "stroke-dasharray=\"7 5\"/></svg> inside"
               "</td></tr></table>");
    }

#ifdef MODULE_FORECAST_ENABLED
    // Last, deliberately: the measured values are what the reader came for and
    // the forecast is the supporting note, so it reads as a footnote rather
    // than as something competing with the two temperatures.
    appendForecastSection(p);
#endif

    appendWeek(p, now);

    p += F("<div class=\"foot\">Measured on site &middot; refreshes every ");
    p += (KINDLE_REFRESH_SEC / 60);
    p += F(" minutes</div></body></html>");

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
