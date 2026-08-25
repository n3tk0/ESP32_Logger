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

// ---------------------------------------------------------------------------
// The trend chart
// ---------------------------------------------------------------------------
// Two series on one set of axes, drawn as SVG path data. The e-ink panel
// cannot distinguish line colours, so outdoor is solid and indoor is dashed.
//
// Gaps (hours with no reading) break the path rather than interpolating
// across them: a flat line through a four-hour outage reads as "it was
// steady", which is a lie the chart should not tell.
static void appendChart(String& out,
                        const TrendRing::Hour* a, const TrendRing::Hour* b,
                        bool haveA, bool haveB) {
    // Shared y-range across both series so the two lines are comparable.
    float lo =  1e9f, hi = -1e9f;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        if (haveA && a[i].count) { if (a[i].min < lo) lo = a[i].min; if (a[i].max > hi) hi = a[i].max; }
        if (haveB && b[i].count) { if (b[i].min < lo) lo = b[i].min; if (b[i].max > hi) hi = b[i].max; }
    }
    if (lo > hi) {
        out += F("<p class=\"note\">No trend data yet &mdash; the 24 hour "
                 "history fills as readings arrive.</p>");
        return;
    }
    // Pad so a flat day is not a line hugging an axis, and never divide by 0.
    float pad = (hi - lo) * 0.15f;
    if (pad < 0.5f) pad = 0.5f;
    lo -= pad; hi += pad;
    const float span = hi - lo;

    const int plotL = 44, plotR = CHART_W - 8;
    const int plotT = 8,  plotB = CHART_H - 22;
    const float dx  = (float)(plotR - plotL) / (float)(TrendRing::HOURS - 1);

    out += F("<svg class=\"chart\" width=\"");
    out += CHART_W;
    out += F("\" height=\"");
    out += CHART_H;
    out += F("\" viewBox=\"0 0 ");
    out += CHART_W; out += ' '; out += CHART_H;
    out += F("\">");

    // Horizontal guides at min / mid / max, each labelled. Three is as many
    // as this height carries without the labels colliding.
    for (int k = 0; k <= 2; k++) {
        const float v = hi - span * (float)k / 2.0f;
        const int   y = plotT + (int)((float)(plotB - plotT) * (float)k / 2.0f);
        out += F("<line class=\"grid\" x1=\""); out += plotL;
        out += F("\" y1=\""); out += y;
        out += F("\" x2=\""); out += plotR;
        out += F("\" y2=\""); out += y; out += F("\"/>");
        char lbl[12];
        fmtInt(lbl, sizeof(lbl), v);
        out += F("<text class=\"axis\" x=\""); out += plotL - 6;
        out += F("\" y=\""); out += y + 4;
        out += F("\" text-anchor=\"end\">"); out += lbl; out += F("</text>");
    }

    // Hour ticks every 6 hours, labelled by how long ago.
    for (int i = 0; i < TrendRing::HOURS; i += 6) {
        const int x = plotL + (int)(dx * (float)i);
        out += F("<text class=\"axis\" x=\""); out += x;
        out += F("\" y=\""); out += CHART_H - 6;
        out += F("\" text-anchor=\"middle\">-");
        out += (TrendRing::HOURS - 1 - i);
        out += F("h</text>");
    }

    struct SeriesSpec { const TrendRing::Hour* h; bool have; const char* cls; };
    const SeriesSpec specs[2] = {
        { a, haveA, "l-out" },
        { b, haveB, "l-in"  },
    };

    for (int s = 0; s < 2; s++) {
        if (!specs[s].have) continue;
        String d;
        bool penDown = false;
        for (int i = 0; i < TrendRing::HOURS; i++) {
            const TrendRing::Hour& bucket = specs[s].h[i];
            if (bucket.count == 0) { penDown = false; continue; }
            const float mean = bucket.sum / (float)bucket.count;
            const int   x    = plotL + (int)(dx * (float)i);
            const int   y    = plotT + (int)((hi - mean) / span
                                             * (float)(plotB - plotT));
            d += penDown ? 'L' : 'M';
            d += x; d += ' '; d += y; d += ' ';
            penDown = true;
        }
        if (d.length()) {
            out += F("<path class=\""); out += specs[s].cls;
            out += F("\" d=\""); out += d; out += F("\"/>");
        }
    }

    out += F("</svg>");
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------
static void handleKindle(AsyncWebServerRequest* req) {
    const Latest outT = latestOf(KINDLE_OUTDOOR_SENSOR, "temperature");
    const Latest outH = latestOf(KINDLE_OUTDOOR_SENSOR, "humidity");
    const Latest outP = latestOf(KINDLE_OUTDOOR_SENSOR, "pressure");
    const Latest inT  = latestOf(KINDLE_INDOOR_SENSOR,  "temperature");
    const Latest inH  = latestOf(KINDLE_INDOOR_SENSOR,  "humidity");

    const uint32_t now = (uint32_t)time(nullptr);

    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn [TrendRing::HOURS];
    const bool haveOut = trendRing.series(KINDLE_OUTDOOR_SENSOR, "temperature", now, tOut);
    const bool haveIn  = trendRing.series(KINDLE_INDOOR_SENSOR,  "temperature", now, tIn);

    String p;
    // Rough final size, reserved up front: String growth on the AsyncTCP task
    // is the one thing here that can fragment the heap.
    p.reserve(6000);

    p += F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=");
    p += PAGE_W;
    p += F("\"><meta http-equiv=\"refresh\" content=\"");
    p += KINDLE_REFRESH_SEC;
    p += F("\"><title>Weather</title><style>"
           // No web fonts: the reader has serif faces built in and anything
           // downloaded is bytes spent for a face that dithers no better.
           "body{font-family:Georgia,serif;margin:0;padding:14px;"
           "background:#fff;color:#000;-webkit-text-size-adjust:none}"
           "*{box-sizing:border-box}"
           ".row{width:100%;border-collapse:collapse;margin-bottom:10px}"
           ".row td{vertical-align:top;padding:0}"
           ".card{border:2px solid #000;padding:10px 12px}"
           ".label{font-size:15px;text-transform:uppercase;letter-spacing:2px;"
           "margin-bottom:2px}"
           // 76px is about as large as fits two digits, a minus sign and a
           // degree mark on a 600px viewport without wrapping.
           ".big{font-size:76px;line-height:1;font-weight:normal}"
           ".unit{font-size:26px}"
           ".sub{font-size:16px;margin-top:4px}"
           "h2{font-size:16px;text-transform:uppercase;letter-spacing:2px;"
           "margin:14px 0 4px;border-bottom:2px solid #000;padding-bottom:3px}"
           ".chart{display:block;margin:0 auto}"
           // Stroke widths, not shades: a 1px mid-grey line dithers into a
           // dotted mess on this panel, so the grid is the only grey and it
           // is deliberately light enough to read as a rule, not a line.
           ".grid{stroke:#999;stroke-width:1}"
           ".axis{font-size:11px;fill:#000;font-family:Georgia,serif}"
           ".l-out{fill:none;stroke:#000;stroke-width:3}"
           ".l-in{fill:none;stroke:#000;stroke-width:2;stroke-dasharray:6 4}"
           ".legend{font-size:13px;text-align:center;margin-top:2px}"
           ".note{font-size:14px;font-style:italic;text-align:center;"
           "padding:26px 0}"
           ".foot{font-size:12px;text-align:center;margin-top:12px;color:#555}"
           ".stale{font-style:italic}"
           "</style></head><body>");

    char buf[16];

    // ── Headline: the two temperatures, side by side ────────────────────────
    p += F("<table class=\"row\"><tr>"
           "<td width=\"50%\" style=\"padding-right:5px\"><div class=\"card\">"
           "<div class=\"label\">Outside</div><div class=\"big\">");
    fmtTemp(buf, sizeof(buf), outT.value);
    p += buf;
    p += F("<span class=\"unit\">&deg;C</span></div>");
    if (outH.ok || outP.ok) {
        p += F("<div class=\"sub\">");
        if (outH.ok) { fmtInt(buf, sizeof(buf), outH.value); p += buf; p += F("% RH"); }
        if (outH.ok && outP.ok) p += F(" &middot; ");
        if (outP.ok) { fmtInt(buf, sizeof(buf), outP.value); p += buf; p += F(" hPa"); }
        p += F("</div>");
    }
    if (!outT.ok) p += F("<div class=\"sub stale\">no data</div>");
    p += F("</div></td>"
           "<td width=\"50%\" style=\"padding-left:5px\"><div class=\"card\">"
           "<div class=\"label\">Inside</div><div class=\"big\">");
    fmtTemp(buf, sizeof(buf), inT.value);
    p += buf;
    p += F("<span class=\"unit\">&deg;C</span></div>");
    if (inH.ok) {
        p += F("<div class=\"sub\">");
        fmtInt(buf, sizeof(buf), inH.value);
        p += buf; p += F("% RH</div>");
    }
    if (!inT.ok) p += F("<div class=\"sub stale\">no data</div>");
    p += F("</div></td></tr></table>");

    // ── Forecast ────────────────────────────────────────────────────────────
#ifdef MODULE_FORECAST_ENABLED
    appendForecastSection(p);
#endif

    // ── 24 hour trend ───────────────────────────────────────────────────────
    p += F("<h2>24 hour temperature</h2>");
    appendChart(p, tOut, tIn, haveOut, haveIn);
    if (haveOut || haveIn) {
        p += F("<div class=\"legend\">&#9473;&#9473; outside &nbsp;&nbsp; "
               "&#9548;&#9548; inside</div>");
    }

    // ── Footer ──────────────────────────────────────────────────────────────
    p += F("<div class=\"foot\">");
    if (now > 1000000000u) {
        struct tm tmv;
        localtime_r((time_t*)&now, &tmv);
        char when[32];
        strftime(when, sizeof(when), "%a %d %b %H:%M", &tmv);
        p += when;
    } else {
        p += F("clock not set");
    }
    p += F(" &middot; refreshes every ");
    p += (KINDLE_REFRESH_SEC / 60);
    p += F(" min</div></body></html>");

    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", p);
    // The refresh is driven by the meta tag, so nothing should be served from
    // cache — an e-ink browser holding a stale page is indistinguishable from
    // a dead sensor.
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
