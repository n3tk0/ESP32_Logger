// ============================================================================
// tests/host/test_kindle_flow.cpp
//
// src/web/KindleFlow.h — where everything on the e-ink page goes, worked out
// for what is on it.
//
// WHAT IS WORTH TESTING HERE
// --------------------------
// Not whether the page looks right — tools/kindle_preview and a photograph of
// the panel answer that. What is worth testing is the arithmetic, because
// every failure of it is a page that renders perfectly and is wrong: a section
// drawn over the one under it, a figure wider than its column (the browser
// clips it; FBInk draws straight over its neighbour), a footer pushed off the
// bottom of an 800 px screen with no scrollbar to say so. So this walks every
// combination of switches and counts and checks those three things, and then
// the handful of pages the change exists for.
//
//   g++ -std=gnu++17 -Wall -Wextra -O1 -g -I tests/host/shims -I.
//       tests/host/test_kindle_flow.cpp -o t && ./t
// ============================================================================
#include <string>
#include <cstring>
#include <cstdio>

#include "src/web/KindleFlow.h"
#include "check.h"

// The widths the default places come to — pressure with its arrow, a dew
// point, a CO2, and the indoor three — so the cases below read as the page.
static unsigned advPress() { return kdFlowWorstAdvance("pressure", "1008", "hPa", true); }
static unsigned advDew()   { return kdFlowWorstAdvance("dew_point", "3.1", "\xC2\xB0", false); }
static unsigned advCo2()   { return kdFlowWorstAdvance("co2", "640", "ppm", false); }
static unsigned advTemp()  { return kdFlowWorstAdvance("temperature", "21.0", "\xC2\xB0", false); }
static unsigned advHum()   { return kdFlowWorstAdvance("humidity", "44", "%", false); }
static unsigned advAqi()   { return kdFlowWorstAdvance("aqi", "42", "", false); }

/// What the dashboard defaults to: pressure and dew point in the grid, the
/// indoor three under the clock — see kdZonesDefault().
static KdFlowIn defaultPage() {
    KdFlowIn in;
    in.nGrid = 2;
    in.gridAdv[0] = (uint16_t)advPress();
    in.gridAdv[1] = (uint16_t)advDew();
    in.nIn = 3;
    in.inAdv[0] = (uint16_t)advTemp();
    in.inAdv[1] = (uint16_t)advHum();
    in.inAdv[2] = (uint16_t)advAqi();
    return in;
}

// ---------------------------------------------------------------------------
// The widths a place is sized by
// ---------------------------------------------------------------------------
static void test_worst_advance_is_the_widest_it_gets() {
    // A temperature is sized for a sign and two digits whatever it reads, so
    // 9.8 and -12.4 give one layout and a warm afternoon does not shrink the
    // grid on the next repaint.
    CHECK_EQ(kdFlowWorstAdvance("temperature", "9.8", "\xC2\xB0", false),
             kdFlowWorstAdvance("temperature", "-12.4", "\xC2\xB0", false));
    // Pressure in hPa is sized for four digits: 999 and 1008 are one layout.
    CHECK_EQ(kdFlowWorstAdvance("pressure", "999", "hPa", false),
             kdFlowWorstAdvance("pressure", "1008", "hPa", false));
    // ...and in mmHg for three.
    CHECK(kdFlowWorstAdvance("pressure", "756", "mmHg", false) <
          kdFlowWorstAdvance("pressure", "1008", "mmHg", false));
    // Humidity for 100.
    CHECK_EQ(kdFlowWorstAdvance("humidity", "44", "%", false),
             kdFlowWorstAdvance("humidity", "100", "%", false));
    // A metric the table does not know is sized by what it reads, and grows
    // with it.
    CHECK(kdFlowWorstAdvance("co2", "640", "ppm", false) <
          kdFlowWorstAdvance("co2", "1240", "ppm", false));
    // The decimals are the place's own.
    CHECK(kdFlowWorstAdvance("temperature", "21", "\xC2\xB0", false) <
          kdFlowWorstAdvance("temperature", "21.0", "\xC2\xB0", false));
    // The unit and the arrow are part of the width.
    CHECK(kdFlowWorstAdvance("pressure", "1008", "hPa", true) >
          kdFlowWorstAdvance("pressure", "1008", "hPa", false));
    CHECK(kdFlowWorstAdvance("pressure", "1008", "hPa", false) >
          kdFlowWorstAdvance("pressure", "1008", "", false));
    // Nothing printed is still something to size.
    CHECK(kdFlowWorstAdvance("aqi", "", "", false) > 0);
}

// ---------------------------------------------------------------------------
// The ordinary page is where it was
// ---------------------------------------------------------------------------
static void test_the_ordinary_page_keeps_its_sections() {
    // Everything on and a forecast: there is no room to give, and the
    // sections land exactly where kindle/layout/600x800.conf puts them.
    const KdFlow f = kdFlowCompute(defaultPage());
    CHECK_EQ(f.topBot, 282);
    CHECK_EQ(f.rule2Y, 282);
    CHECK_EQ(f.grY, 308);
    CHECK_EQ(f.grH, 220);
    CHECK_EQ(f.rule3Y, 552);
    CHECK_EQ(f.wkRuleY, 676);
    CHECK_EQ(f.heroSz, 88);
    CHECK_EQ(f.bigSz, 44);
    CHECK_EQ(f.clSize, 96);
    CHECK_EQ(f.labSz, 14);
    CHECK_EQ(f.subY, 128);
    CHECK_EQ(f.sepH, 252);
    CHECK_EQ(f.inRuleY, 124);
    CHECK_EQ(f.inLabY, 134);
    // But the two grid readings are no longer the same size as four would
    // be: one row, set as large as a half-column takes.
    CHECK_EQ(f.gridNRows, 1);
    CHECK_EQ(f.gridRows[0], 2);
    CHECK(f.gridValSz > 34);
}

// ---------------------------------------------------------------------------
// The pages the change is for
// ---------------------------------------------------------------------------
static void test_no_week_strip_moves_everything_down() {
    KdFlowIn in = defaultPage();
    in.week = false;
    const KdFlow f = kdFlowCompute(in);
    // The 88 px the strip had are given back: the forecast sits on the footer,
    // and the readings above took the space.
    CHECK_EQ(f.wkRuleY, KDF_FOOT_Y);
    CHECK_EQ(f.rule3Y + KDF_FC_H, KDF_FOOT_Y);
    const KdFlow o = kdFlowCompute(defaultPage());
    CHECK(f.topBot > o.topBot);
    CHECK(f.heroSz > o.heroSz);
    CHECK(f.grH >= KDF_CHART_MIN);
}

static void test_no_forecast_is_the_old_standalone_and_more() {
    KdFlowIn in = defaultPage();
    in.forecast = false;
    const KdFlow f = kdFlowCompute(in);
    // The headline and the clock grow to the standalone page's sizes...
    CHECK(f.heroSz >= 103);
    CHECK(f.clSize >= 109);
    // ...the chart keeps its height and ends on the week strip's rule...
    CHECK_EQ(f.rule3Y, 676);
    CHECK(f.grH >= KDF_CHART_MIN);
    // ...and the two grid readings go one under the other, larger than they
    // could ever be side by side.
    CHECK_EQ(f.gridNRows, 2);
    CHECK_EQ(f.gridRows[0], 1);
    CHECK_EQ(f.gridRows[1], 1);
    CHECK(f.gridValSz >= 55);
}

static void test_readings_first_then_the_chart() {
    // With the forecast and the week both off there is more room than the
    // readings can use. They take what makes their type grow, and the chart
    // gets the rest.
    KdFlowIn in = defaultPage();
    in.forecast = false;
    in.week = false;
    const KdFlow f = kdFlowCompute(in);
    const KdFlowIn one = [] { KdFlowIn x = defaultPage(); x.forecast = false; return x; }();
    const KdFlow g = kdFlowCompute(one);
    CHECK_EQ(f.heroSz, g.heroSz);                // the readings were already at their largest
    CHECK(f.grH > g.grH + 60);                   // so the chart got the week's 88
    CHECK_EQ(f.grY + f.grH + KDF_CHART_BELOW, KDF_FOOT_Y);
}

static void test_no_chart_gives_the_readings_everything() {
    KdFlowIn in = defaultPage();
    in.chart = false;
    const KdFlow f = kdFlowCompute(in);
    CHECK_EQ(f.grH, 0);
    CHECK_EQ(f.rule2Y, f.rule3Y);
    CHECK_EQ(f.rule3Y, 552);                     // straight into the forecast
    CHECK(f.topBot == 552);
}

static void test_four_readings_fill_two_rows() {
    KdFlowIn in = defaultPage();
    in.nGrid = 4;
    in.gridAdv[2] = (uint16_t)advCo2();
    in.gridAdv[3] = (uint16_t)advHum();
    const KdFlow f = kdFlowCompute(in);
    CHECK_EQ(f.gridNRows, 2);
    CHECK_EQ(f.gridRows[0], 2);
    CHECK_EQ(f.gridRows[1], 2);
    // Two full rows is the ordinary page's grid, at its size.
    CHECK_EQ(f.gridValSz, 34);
}

static void test_two_indoor_fields_take_the_row() {
    KdFlowIn in = defaultPage();
    in.nIn = 2;
    const KdFlow two = kdFlowCompute(in);
    const KdFlow three = kdFlowCompute(defaultPage());
    CHECK(two.inValSz1 > three.inValSz1);
    // The first field gets what it needs, not a fixed share.
    CHECK(two.inW1Pm > 500 && two.inW1Pm < 900);
}

// ---------------------------------------------------------------------------
// Every combination
// ---------------------------------------------------------------------------
static int g_cases = 0;

/// Everything that must hold whatever is on the page.
static void checkPage(const KdFlowIn& in, const KdFlow& f) {
    g_cases++;
    // ── The sections chain, top to bottom, and end on the footer ──
    CHECK(f.topBot - KDF_TOP_Y >= KDF_TOP_MIN);
    CHECK_EQ(f.rule2Y, f.topBot);
    if (in.chart) {
        CHECK_EQ(f.grY, f.rule2Y + KDF_CHART_ABOVE);
        CHECK(f.grH >= KDF_CHART_MIN);
        CHECK_EQ(f.grY + f.grH + KDF_CHART_BELOW, f.rule3Y);
    } else {
        CHECK_EQ(f.rule3Y, f.rule2Y);
        CHECK_EQ(f.grH, 0);
    }
    CHECK_EQ(f.rule3Y + (in.forecast ? KDF_FC_H : 0), f.wkRuleY);
    CHECK_EQ(f.wkRuleY + (in.week ? KDF_WEEK_H : 0), KDF_FOOT_Y);

    // ── The headline and the clock never outgrow what was measured to fit ──
    CHECK(f.heroSz >= 88 && f.heroSz <= 104);
    CHECK(f.clSize >= 96 && f.clSize <= 110);
    CHECK(f.subY > f.heroY + f.heroSz);

    const int bot = f.topBot - 8;

    // ── The grid ──
    int cells = 0;
    for (int r = 0; r < f.gridNRows; r++) {
        CHECK(f.gridRows[r] >= 1 && f.gridRows[r] <= 3);
        cells += f.gridRows[r];
    }
    CHECK_EQ(cells, in.nGrid);
    if (in.nGrid) {
        // Under the line beneath the headline, and inside the block.
        CHECK(f.gridY >= (in.sub ? f.subY + f.subSz : f.subY));
        CHECK(f.gridY + (f.gridNRows - 1) * f.gridRowH + f.labSz + 4 + f.gridValSz <= bot);
        // A caption and its value fit their row, with a gap before the next.
        CHECK(f.labSz + 4 + f.gridValSz + KDF_GRID_GAP <= f.gridRowH);
        // Never larger than six tenths of the headline.
        CHECK(f.gridValSz <= f.heroSz * 60 / 100);
        // Every cell fits its column, or is at the size the ordinary page
        // measured as fitting — the estimate is pessimistic by design.
        int at = 0;
        for (int r = 0; r < f.gridNRows; r++) {
            const int cellW = KDF_COL_L / f.gridRows[r] - KDF_CELL_PAD;
            const int floorV = (f.gridRows[0] >= 3) ? 27 : 34;
            for (int c = 0; c < f.gridRows[r]; c++, at++) {
                const int w = f.gridValSz * in.gridAdv[at] / 1000;
                CHECK(w <= cellW || f.gridValSz <= floorV);
            }
        }
    } else {
        CHECK_EQ(f.gridNRows, 0);
    }

    // ── The indoor row ──
    CHECK(f.inRuleY > KDF_CL_Y + f.clSize);
    if (in.nIn) {
        CHECK(f.inValSz1 > 0);
        CHECK(f.inValY >= f.inLabY + f.labSz + 18);
        CHECK(f.inValSz1 <= f.heroSz * 80 / 100);
        if (f.inStack) {
            CHECK(in.nIn >= 2);
            CHECK(f.inVal2Y >= f.inValY + f.inValSz1 + f.labSz + 4);
            CHECK(f.inVal2Y + f.inValSz <= bot);
            CHECK(f.inValSz1 * in.inAdv[0] / 1000 <= KDF_COL_R - KDF_CELL_PAD);
        } else {
            CHECK(f.inValY + f.inValSz1 <= bot);
            CHECK_EQ(f.inVal2Y, f.inValY + f.inValSz1 - f.inValSz);
            // The widths the row was divided by hold what goes in them.
            if (in.nIn > 1) {
                const int w1 = KDF_COL_R * f.inW1Pm / 1000;
                CHECK(f.inValSz1 * in.inAdv[0] / 1000 <= w1 || f.inValSz1 <= 52);
            } else {
                CHECK_EQ(f.inW1Pm, 1000);
            }
        }
    } else {
        CHECK_EQ(f.inValSz1, 0);
    }
    CHECK_EQ(f.sepH, f.topBot - KDF_TOP_Y - 10);
}

static void test_every_combination() {
    const unsigned grid[6] = { advPress(), advDew(), advCo2(), advHum(), advAqi(), advTemp() };
    const unsigned wide[6] = { 3600, 3400, 3200, 3000, 2800, 2600 };   // long units, big numbers
    const unsigned ind[3]  = { advTemp(), advHum(), advAqi() };
    for (int mask = 0; mask < 16; mask++)
    for (int set = 0; set < 2; set++)
    for (int ng = 0; ng <= 6; ng++)
    for (int ni = 0; ni <= 3; ni++) {
        KdFlowIn in;
        in.chart    = mask & 1;
        in.forecast = mask & 2;
        in.week     = mask & 4;
        in.sub      = mask & 8;
        in.nGrid = (uint8_t)ng;
        for (int i = 0; i < ng; i++) in.gridAdv[i] = (uint16_t)(set ? wide[i] : grid[i]);
        in.nIn = (uint8_t)ni;
        for (int i = 0; i < ni; i++) in.inAdv[i] = (uint16_t)(set ? wide[i] : ind[i]);
        const KdFlow f = kdFlowCompute(in);
        checkPage(in, f);
        // The browser page: the same, less what its sections cost over the
        // panel's — out of the chart when there is one, else the top block.
        const KdFlow h = kdFlowComputeHtml(in);
        if (in.chart) {
            CHECK_EQ(h.topBot, f.topBot);
            CHECK_EQ(kdFlowHtmlChartH(h), f.grH - kdFlowHtmlExtra(h));
            CHECK(kdFlowHtmlChartH(h) >= 150);
        } else {
            CHECK_EQ(h.topBot, f.topBot - kdFlowHtmlExtra(h));
            CHECK_EQ(kdFlowHtmlChartH(h), 0);
        }
        // The indoor fields on one line have room for their captions too.
        if (ni > 1 && !f.inStack)
            CHECK(kdFlowInNeed(f.inValSz1, f.inValSz1 ? in.inAdv[0] : 1000, in.inAdv, ni) <=
                  KDF_COL_R || f.inValSz1 <= 40);
    }
    std::printf("  %d pages checked\n", g_cases);
}

static void test_switching_a_section_off_never_shrinks_anything() {
    for (int mask = 0; mask < 8; mask++) {
        KdFlowIn in = defaultPage();
        in.chart = mask & 1; in.forecast = mask & 2; in.week = mask & 4;
        const KdFlow f = kdFlowCompute(in);
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (!(mask & bit)) continue;
            KdFlowIn less = in;
            if (bit == 1) less.chart = false;
            if (bit == 2) less.forecast = false;
            if (bit == 4) less.week = false;
            const KdFlow g = kdFlowCompute(less);
            CHECK(g.heroSz >= f.heroSz);
            CHECK(g.clSize >= f.clSize);
            CHECK(g.gridValSz >= f.gridValSz);
            CHECK(g.inValSz1 >= f.inValSz1);
            CHECK(g.topBot >= f.topBot);
        }
    }
}

// ---------------------------------------------------------------------------
// What the two renderers are sent
// ---------------------------------------------------------------------------
static int keyOf(const KdFlowKV* kv, int n, const char* key) {
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, key)) return kv[i].value;
    return -9999;
}

static void test_panel_keys() {
    const KdFlow f = kdFlowCompute(defaultPage());
    KdFlowKV kv[KDF_PANEL_KEYS];
    const int n = kdFlowPanelKeys(f, 600, kv);
    CHECK_EQ(n, KDF_PANEL_KEYS);
    // At 600 they are the design's own numbers — the ordinary page's layout
    // file, key for key, where nothing moved.
    CHECK_EQ(keyOf(kv, n, "RULE2_Y"), 282);
    CHECK_EQ(keyOf(kv, n, "LAB_CHART_Y"), 288);
    CHECK_EQ(keyOf(kv, n, "GR_Y"), 308);
    CHECK_EQ(keyOf(kv, n, "GR_H"), 220);
    CHECK_EQ(keyOf(kv, n, "KEY_Y"), 530);
    CHECK_EQ(keyOf(kv, n, "RULE3_Y"), 552);
    CHECK_EQ(keyOf(kv, n, "LAB_FC_Y"), 558);
    CHECK_EQ(keyOf(kv, n, "FC_ICON_Y"), 580);
    CHECK_EQ(keyOf(kv, n, "FC_TEMP_Y"), 614);
    CHECK_EQ(keyOf(kv, n, "FC_WIND_Y"), 652);
    CHECK_EQ(keyOf(kv, n, "OL0_Y"), 564);
    CHECK_EQ(keyOf(kv, n, "WK_HDG_RULE_Y"), 676);
    CHECK_EQ(keyOf(kv, n, "WK_HDG_Y"), 681);
    CHECK_EQ(keyOf(kv, n, "WK_Y"), 700);
    CHECK_EQ(keyOf(kv, n, "HERO_SZ"), 88);
    CHECK_EQ(keyOf(kv, n, "CL_SIZE"), 96);
    CHECK_EQ(keyOf(kv, n, "CL_H"), 97);
    CHECK_EQ(keyOf(kv, n, "IN_RULE_Y"), 124);
    CHECK_EQ(keyOf(kv, n, "FC_BAND"), 1);
    // Every key is a name the reader's list carries — no two the same.
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) CHECK(strcmp(kv[i].key, kv[j].key) != 0);

    // At 1072 every position and size scales with the panel, as the 1072
    // layout file does; the share and the switches do not.
    KdFlowKV big[KDF_PANEL_KEYS];
    kdFlowPanelKeys(f, 1072, big);
    CHECK_EQ(keyOf(big, n, "RULE2_Y"), 504);     // 1072x1448.conf's own RULE2_Y
    CHECK_EQ(keyOf(big, n, "RULE3_Y"), 986);
    CHECK_EQ(keyOf(big, n, "WK_Y"), 1251);
    CHECK_EQ(keyOf(big, n, "HERO_SZ"), 157);
    CHECK_EQ(keyOf(big, n, "IN_W1"), keyOf(kv, n, "IN_W1"));
    CHECK_EQ(keyOf(big, n, "FC_BAND"), 1);

    char rows[16];
    kdFlowRowsText(f, rows, sizeof(rows));
    CHECK_STREQ(rows, "2");
    KdFlowIn in = defaultPage();
    in.forecast = false;
    kdFlowRowsText(kdFlowCompute(in), rows, sizeof(rows));
    CHECK_STREQ(rows, "1 1");
    in.nGrid = 0;
    kdFlowRowsText(kdFlowCompute(in), rows, sizeof(rows));
    CHECK_STREQ(rows, "");
}

struct Css {
    std::string s;
    Css& operator+=(const char* c) { s += c;                 return *this; }
    Css& operator+=(int n)         { s += std::to_string(n); return *this; }
    bool has(const char* needle) const { return s.find(needle) != std::string::npos; }
};

static void test_page_css() {
    const KdFlow f = kdFlowCompute(defaultPage());
    auto ident = [](int v) { return v; };
    Css c;
    kdFlowCss(c, f, 0, ident);
    CHECK(c.has(".v1{font-size:88px}"));
    CHECK(c.has(".col-l,.col-r{height:250px}"));
    CHECK(c.has(".clock{font-size:96px;line-height:100px}"));
    CHECK(c.has(".grid td{vertical-align:middle;height:115px}"));
    // Each clock style gets its own arm, at this layout's size.
    Css boxed;  kdFlowCss(boxed, f, 1, ident);
    CHECK(boxed.has(".clock{height:96px;line-height:96px;font-size:84px}"));
    Css dated;  kdFlowCss(dated, f, 3, ident);
    CHECK(dated.has(".clock-d{height:24px;font-size:15px}"));
    // Scaled by what the firmware passes — kdPx() at another page width.
    Css wide;
    kdFlowCss(wide, f, 0, [](int v) { return (v * 1072 + 300) / 600; });
    CHECK(wide.has(".v1{font-size:157px}"));
    // No grid, no grid rules.
    KdFlowIn in = defaultPage();
    in.nGrid = 0;
    Css none;
    kdFlowCss(none, kdFlowCompute(in), 0, ident);
    CHECK(!none.has(".gv"));
    CHECK_EQ(kdFlowHtmlChartH(kdFlowCompute(defaultPage())), 220 - 50);
}

int main() {
    RUN(test_worst_advance_is_the_widest_it_gets);
    RUN(test_the_ordinary_page_keeps_its_sections);
    RUN(test_no_week_strip_moves_everything_down);
    RUN(test_no_forecast_is_the_old_standalone_and_more);
    RUN(test_readings_first_then_the_chart);
    RUN(test_no_chart_gives_the_readings_everything);
    RUN(test_four_readings_fill_two_rows);
    RUN(test_two_indoor_fields_take_the_row);
    RUN(test_every_combination);
    RUN(test_switching_a_section_off_never_shrinks_anything);
    RUN(test_panel_keys);
    RUN(test_page_css);
    return SUMMARY();
}
