// ============================================================================
// tools/kindle_preview/flow_dump.cpp — src/web/KindleFlow.h, from a shell
//
// The layout the collector works out for a page, printed for the three things
// that need it without a device: preview.py, which draws the browser page;
// tests/kindle/drive_dash.sh, which feeds the panel script a payload; and
// tools/check_kindle_flow_parity.py, which holds the settings page's port of
// the same rules to this one.
//
//   g++ -std=gnu++17 -I. tools/kindle_preview/flow_dump.cpp -o flow_dump
//   ./flow_dump kv   chart=1 fc=0 week=1 sub=1 grid=3135,2202 in=2202,1836 res=600
//   ./flow_dump css  ... clock=2 pagew=600 html=1
//   ./flow_dump json ...
//   ./flow_dump adv  'pressure:1008:hPa:1;dew_point:-3.1:°'
//
// grid= and in= are each place's kdFlowWorstAdvance(), comma-separated; or
// place=metric:text:unit[:arrow] pairs through gridp= and inp=, which are
// measured here the way the collector measures them.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "src/web/KindleFlow.h"

struct Css {
    std::string s;
    Css& operator+=(const char* c) { s += c;                 return *this; }
    Css& operator+=(int n)         { s += std::to_string(n); return *this; }
};

static int parseList(const char* v, uint16_t* out, int max) {
    int n = 0;
    while (*v && n < max) {
        out[n++] = (uint16_t)strtoul(v, (char**)&v, 10);
        if (*v == ',') v++;
    }
    return n;
}

/// "pressure:1008:hPa:1;dew_point:3.1:°" -> each place's advance.
static int parsePlaces(const char* v, uint16_t* out, int max) {
    int n = 0;
    std::string all(v);
    size_t at = 0;
    while (at <= all.size() && n < max) {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        std::string one = all.substr(at, end - at);
        if (!one.empty()) {
            std::string f[4];
            size_t p = 0;
            for (int i = 0; i < 4; i++) {
                size_t c = one.find(':', p);
                if (c == std::string::npos) { f[i] = one.substr(p); p = one.size() + 1; break; }
                f[i] = one.substr(p, c - p);
                p = c + 1;
            }
            out[n++] = (uint16_t)kdFlowWorstAdvance(f[0].c_str(), f[1].c_str(),
                                                   f[2].c_str(), f[3] == "1");
        }
        at = end + 1;
    }
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: flow_dump kv|css|json [key=value ...]\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "adv") {
        // flow_dump adv "pressure:1008:hPa:1;dew_point:3.1:°" -> one per line
        for (int i = 2; i < argc; i++) {
            uint16_t a[64];
            const int n = parsePlaces(argv[i], a, 64);
            for (int k = 0; k < n; k++) printf("%u\n", (unsigned)a[k]);
        }
        return 0;
    }
    KdFlowIn in;
    unsigned res = 600, pagew = 600;
    int clock = 0;
    bool html = false;
    for (int i = 2; i < argc; i++) {
        const char* a = argv[i];
        const char* eq = strchr(a, '=');
        if (!eq) continue;
        const std::string k(a, eq - a);
        const char* v = eq + 1;
        if      (k == "chart") in.chart    = atoi(v) != 0;
        else if (k == "fc")    in.forecast = atoi(v) != 0;
        else if (k == "week")  in.week     = atoi(v) != 0;
        else if (k == "sub")   in.sub      = atoi(v) != 0;
        else if (k == "grid")  in.nGrid    = (uint8_t)parseList(v, in.gridAdv, 6);
        else if (k == "in")    in.nIn      = (uint8_t)parseList(v, in.inAdv, 3);
        else if (k == "gridp") in.nGrid    = (uint8_t)parsePlaces(v, in.gridAdv, 6);
        else if (k == "inp")   in.nIn      = (uint8_t)parsePlaces(v, in.inAdv, 3);
        else if (k == "res")   res   = (unsigned)atoi(v);
        else if (k == "pagew") pagew = (unsigned)atoi(v);
        else if (k == "clock") clock = atoi(v);
        else if (k == "html")  html  = atoi(v) != 0;
    }
    const KdFlow f = html ? kdFlowComputeHtml(in) : kdFlowCompute(in);

    if (mode == "kv") {
        KdFlowKV kv[KDF_PANEL_KEYS];
        const int n = kdFlowPanelKeys(f, res, kv);
        for (int i = 0; i < n; i++) printf("LY_%s=%d\n", kv[i].key, kv[i].value);
        char rows[16];
        kdFlowRowsText(f, rows, sizeof(rows));
        printf("LY_GRID_ROWS=\"%s\"\n", rows);
        return 0;
    }
    if (mode == "css") {
        Css c;
        kdFlowCss(c, f, (uint8_t)clock, [pagew](int v) {
            return v >= 0 ? (int)((v * (long)pagew + 300) / 600)
                          : -(int)((-v * (long)pagew + 300) / 600);
        });
        printf("%s\n", c.s.c_str());
        return 0;
    }
    if (mode == "json") {
        printf("{");
        printf("\"topBot\":%d,\"grow\":%d,\"labSz\":%d,\"heroSz\":%d,\"bigSz\":%d,"
               "\"headGap\":%d,\"slashW\":%d,\"subSz\":%d,\"heroY\":%d,\"subY\":%d,",
               f.topBot, f.grow, f.labSz, f.heroSz, f.bigSz, f.headGap, f.slashW,
               f.subSz, f.heroY, f.subY);
        printf("\"gridRows\":[");
        for (int r = 0; r < f.gridNRows; r++) printf(r ? ",%d" : "%d", f.gridRows[r]);
        printf("],\"gridY\":%d,\"gridRowH\":%d,\"gridValSz\":%d,",
               f.gridY, f.gridRowH, f.gridValSz);
        printf("\"clSize\":%d,\"clBoxed\":%d,\"clRuled\":%d,\"clRuledPad\":%d,"
               "\"clDated\":%d,\"clDateSz\":%d,\"clDateGap\":%d,\"clGrow\":%d,\"clH\":%d,",
               f.clSize, f.clBoxed, f.clRuled, f.clRuledPad, f.clDated, f.clDateSz,
               f.clDateGap, f.clGrow, f.clH);
        printf("\"inRuleY\":%d,\"inLabY\":%d,\"inValY\":%d,\"inVal2Y\":%d,"
               "\"inValSz1\":%d,\"inValSz\":%d,\"inW1Pm\":%d,\"inStack\":%s,\"sepH\":%d,",
               f.inRuleY, f.inLabY, f.inValY, f.inVal2Y, f.inValSz1, f.inValSz,
               f.inW1Pm, f.inStack ? "true" : "false", f.sepH);
        printf("\"chart\":%s,\"forecast\":%s,\"week\":%s,\"rule2Y\":%d,\"grY\":%d,"
               "\"grH\":%d,\"rule3Y\":%d,\"wkRuleY\":%d,\"htmlChartH\":%d,\"htmlColH\":%d",
               f.chart ? "true" : "false", f.forecast ? "true" : "false",
               f.week ? "true" : "false", f.rule2Y, f.grY, f.grH, f.rule3Y, f.wkRuleY,
               kdFlowHtmlChartH(f), kdFlowHtmlColH(f));
        printf("}\n");
        return 0;
    }
    fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
