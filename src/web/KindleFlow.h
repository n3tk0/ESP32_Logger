// ============================================================================
// src/web/KindleFlow.h — where everything on the e-ink page goes, worked out
// for what is actually on it
//
// The page used to be two hand-measured layouts: the ordinary one, and the
// "standalone" one a collector with no forecast draws, where the forecast band
// goes and the readings grow into it. Everything else was a fixed coordinate.
// So a reader who switched the week strip off got 88 px of white above the
// footer; two readings in the outdoor grid sat on one row with an empty row
// under them, at the same size as four; the indoor row left a band of white
// under itself whatever it held. Each of those is a place where the page had
// room and did not use it.
//
// THIS WORKS THE LAYOUT OUT INSTEAD, on every render, from what is on the page:
// which sections are switched on, whether there is a forecast, how many places
// in the grid and the indoor row have a reading, and how wide the widest thing
// each of them can print is. The answer goes to both renderers — the browser
// page as a block of CSS after the design's own sheet, the FBInk panel as
// LY_* keys in /kindle/data that it lays over its layout file — so the two
// cannot disagree about where a thing is. The collector is the only end that
// knows all of the inputs at once, which is why it is decided here.
//
// THE RULES, in the order they apply:
//
//   1. The sections below the readings are fixed height and stacked from the
//      bottom: the footer, the week strip, the forecast band. Each one that is
//      not on the page gives its height back.
//   2. The space left over goes to the READINGS FIRST, then to the chart. The
//      top block takes height only while it can turn it into larger type; the
//      moment every figure in it has stopped growing, the rest goes to the
//      chart, which is the one thing on the page that is happy to be any
//      height. With the chart switched off the readings take it all.
//   3. The headline, the clock and the captions grow together, up to what the
//      design measured as the most they can take: the standalone page's sizes,
//      a sixth larger. Past that they are limited by the WIDTH of their column,
//      not the height, and a size a third larger overruns on the day the
//      pressure goes to four digits.
//   4. The grid tries every way of breaking its readings into rows — two side
//      by side or one under the other, three across or two and one — and keeps
//      the one that sets them largest. All of its cells are one size, so the
//      numbers do not jump about from cell to cell; ties go to more rows, which
//      is the one that fills the height.
//   5. The indoor row divides its width by what each field needs rather than by
//      fixed percentages, and may put its first field on a line of its own when
//      that sets it larger.
//
// EVERY NUMBER HERE IS A 600 x 800 DESIGN PIXEL, the unit kindle/layout/
// 600x800.conf and the KD_N() sheet are written in. The panel's values are
// scaled by resW/600 when they are sent, which is exactly how the 1072 layout
// file relates to the 600 one; the browser's by kdPx().
//
// NO ARDUINO IN HERE. tests/host/test_kindle_flow.cpp walks every combination
// of switches and counts, and www/js/kindle.js carries a port of the same rules
// for the settings page's preview — tools/check_kindle_flow_parity.py runs the
// two side by side on the same cases, so a rule changed here and not there
// fails CI rather than drawing a preview of a page the device does not draw.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// How wide a string comes out
// ---------------------------------------------------------------------------
/// How wide a string comes out, in THOUSANDTHS OF THE TYPE SIZE.
///
/// For the shell renderer, which draws with FBInk and cannot ask it how wide it
/// drew something. The headline sets two values and a slash on one baseline, so
/// the second one's x depends on the first one's width, and a script counting
/// characters would be wrong twice over: `${#var}` counts bytes, so "8.4°" is
/// five of them, and a digit and a full stop are not the same width anyway.
///
/// And now for the layout below, which sizes each reading to the widest thing
/// it can print in the column it has.
///
/// The weights are for a serif at a glance size — Bookerly, Caecilia, Georgia,
/// which is the stack this page names. Figures are tabular in all three, hence
/// one width for all ten. Being a few per cent out moves the slash by a pixel
/// or two, which is why an estimate is good enough here and would not be for
/// anything that had to line up.
static inline unsigned kdAdvanceMille(const char* s) {
    if (!s) return 0;
    unsigned total = 0;
    for (const unsigned char* q = (const unsigned char*)s; *q; q++) {
        if ((*q & 0xC0) == 0x80) continue;          // a UTF-8 continuation byte
        unsigned w;
        if (*q == 0xC2) {
            // The two Latin-1 supplement glyphs this page actually prints.
            const unsigned char n = q[1];
            w = (n == 0xB0) ? 330u :                // ° — narrow
                (n == 0xB5) ? 520u : 550u;          // µ
        }
        else if (*q >= 0xC0)                 w = 620;   // a letter outside ASCII
        else if (*q >= '0' && *q <= '9')     w = 500;   // tabular figures
        else if (*q == '.' || *q == ',' ||
                 *q == ':' || *q == '\'')    w = 260;
        else if (*q == '-' || *q == '+' ||
                 *q == '/')                  w = 330;
        else if (*q == ' ')                  w = 250;
        else if (*q == '%')                  w = 800;
        else if (*q >= 'A' && *q <= 'Z')     w = 620;
        else if (*q >= 'a' && *q <= 'z')     w = 500;
        else                                 w = 550;
        total += w;
    }
    return total;
}

/// The fewest integer digits a metric is sized for, whatever it reads now.
///
/// SIZED FOR THE WIDEST IT GETS, NOT FOR TODAY'S NUMBER. A layout sized to the
/// current reading changes the moment the reading does: 9.8° to 10.1° would
/// shrink every figure in the grid on the next repaint, and a pressure crossing
/// 1000 hPa would do it twice a week. So each metric is sized for the width it
/// can reach — a temperature for a sign and two digits, a humidity for 100 —
/// and grows past that only when the reading itself does.
static inline int kdFlowMinDigits(const char* metric, const char* unit) {
    if (!metric) return 0;
    if (!strcmp(metric, "temperature") || !strcmp(metric, "dew_point")) return 2;
    if (!strcmp(metric, "humidity") || !strcmp(metric, "humidity_amb") ||
        !strcmp(metric, "soil_moisture") || !strcmp(metric, "battery_percent") ||
        !strcmp(metric, "wind_direction")) return 3;
    if (!strcmp(metric, "pressure")) {
        if (unit && !strcmp(unit, "mmHg")) return 3;
        if (unit && !strcmp(unit, "inHg")) return 2;
        return 4;
    }
    // The rest by the range they live in: a CO2 reading around 1000 ppm, or
    // light around 10 000 lx, would otherwise resize the page each time it
    // crossed the power of ten — and a panel repaints whole when it does.
    if (!strcmp(metric, "co2") || !strcmp(metric, "eco2") ||
        !strcmp(metric, "tvoc")) return 4;
    if (!strcmp(metric, "lux")) return 5;
    if (!strcmp(metric, "aqi") || !strcmp(metric, "pm1") || !strcmp(metric, "pm25") ||
        !strcmp(metric, "pm4") || !strcmp(metric, "pm10") ||
        !strcmp(metric, "battery_days")) return 3;
    if (!strcmp(metric, "rain") || !strcmp(metric, "rain_rate") ||
        !strcmp(metric, "rain_total") || !strcmp(metric, "wind") ||
        !strcmp(metric, "wind_speed") || !strcmp(metric, "flow_rate") ||
        !strcmp(metric, "uva") || !strcmp(metric, "uvb")) return 2;
    return 0;
}

/// Whether a metric can go below zero, and so needs room for the sign.
static inline bool kdFlowSigned(const char* metric) {
    return metric && (!strcmp(metric, "temperature") || !strcmp(metric, "dew_point"));
}

/// The widest a place's value, unit and arrow come out, in thousandths of the
/// VALUE's type size — the three are drawn at three sizes, and this is the
/// sum in the value's terms: the unit at 0.42 of it (the degree at 0.34), the
/// tendency arrow at 0.5 with its 0.15 em of padding.
///
/// `text` is what the place prints now, already at its decimals.
static inline unsigned kdFlowWorstAdvance(const char* metric, const char* text,
                                          const char* unit, bool arrow) {
    char worst[24];
    size_t at = 0;
    const char* p = text ? text : "";
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); p++; }
    int intDigits = 0;
    const char* q = p;
    while (*q >= '0' && *q <= '9') { intDigits++; q++; }
    int need = kdFlowMinDigits(metric, unit);
    if (intDigits > need) need = intDigits;
    if (need < 1) need = 1;
    if (neg || kdFlowSigned(metric)) worst[at++] = '-';
    for (int i = 0; i < need && at < sizeof(worst) - 1; i++) worst[at++] = '0';
    // Whatever follows the integer part — the decimals — as it is printed.
    for (; *q && at < sizeof(worst) - 1; q++) worst[at++] = *q;
    worst[at] = '\0';

    unsigned adv = kdAdvanceMille(worst);
    if (unit && *unit) {
        if (!strcmp(unit, "\xC2\xB0"))   adv += 330u * 34u / 100u;      // the degree
        else if (!strcmp(unit, "%"))     adv += 800u * 42u / 100u;
        else                             adv += (250u + kdAdvanceMille(unit)) * 42u / 100u;
    }
    if (arrow) adv += 350u;
    return adv;
}

// ---------------------------------------------------------------------------
// The design's fixed numbers
// ---------------------------------------------------------------------------
// Each one is a figure kindle/layout/600x800.conf already carries, named here
// once. The ordinary page — everything switched on, a forecast, the chart — is
// the case where the flow has no room to give, and it lands the sections
// exactly where that file puts them.
static const int KDF_PAGE_H      = 800;
static const int KDF_TOP_Y       = 20;    ///< TOP_Y: the first heading
static const int KDF_FOOT_Y      = 764;   ///< FOOT_RULE_Y: the footer is 36 px, always
static const int KDF_WEEK_H      = 88;    ///< WK_HDG_RULE_Y 676 .. 764
static const int KDF_FC_H        = 124;   ///< RULE3_Y 552 .. 676
static const int KDF_CHART_ABOVE = 26;    ///< RULE2_Y .. GR_Y: the rule and the caption
static const int KDF_CHART_BELOW = 24;    ///< GR_Y+GR_H .. RULE3_Y: the key
static const int KDF_CHART_MIN   = 220;   ///< GR_H: the chart never gets shorter
static const int KDF_TOP_MIN     = 262;   ///< TOP_Y .. RULE2_Y on the ordinary page
static const int KDF_TOP_GROWN   = 386;   ///< where the headline stops growing (the old standalone page)
static const int KDF_COL_L       = 270;   ///< COL_L_W
/// The right column, as the NARROWER of the two renderers has it: FBInk's is
/// 264, the browser's is its half of the page less the 30 px gutter by the
/// rule. Sizing to the narrower is what keeps the page from clipping a figure
/// the panel would have fitted.
static const int KDF_COL_R       = 252;
static const int KDF_CL_Y        = 26;    ///< CL_Y
static const int KDF_HERO_Y      = 38;    ///< HERO_Y
static const int KDF_CELL_PAD    = 6;     ///< the gutter a cell keeps on its right
/// The narrowest an indoor field beside the first can be: its caption, five
/// spaced capitals at the caption size ("ВЛАГА").
static const int KDF_IN_CAP_W = 66;
static const int KDF_GRID_GAP    = 6;     ///< between one grid row and the next

/// How much larger the headline, the clock and the captions may grow, in
/// thousandths — the standalone page's figures over the ordinary page's.
static const int KDF_GROW_MAX    = 1180;  ///< the headline: 88 -> 104
static const int KDF_GROW_CLOCK  = 1146;  ///< the clock: 96 -> 110, the most "17:40" fits in 264
static const int KDF_GROW_BIG    = 1090;  ///< the value beside it: 44 -> 48
static const int KDF_GROW_SUB    = 1120;  ///< the line under it: 17 -> 19

// ---------------------------------------------------------------------------
// The layout
// ---------------------------------------------------------------------------
/// What the layout is worked out from.
struct KdFlowIn {
    bool    chart    = true;    ///< KSHOW_CHART
    bool    forecast = true;    ///< a forecast band is drawn (not standalone)
    bool    week     = true;    ///< KSHOW_WEEK
    bool    sub      = true;    ///< the line under the headline has something in it
    uint8_t nGrid    = 0;       ///< grid places with a reading, 0..6
    uint16_t gridAdv[6] = {0, 0, 0, 0, 0, 0};   ///< kdFlowWorstAdvance() of each
    uint8_t nIn      = 0;       ///< indoor places with a reading, 0..3
    uint16_t inAdv[3]   = {0, 0, 0};
};

/// Where everything goes, in design pixels.
struct KdFlow {
    // ── The top block ──
    int16_t topBot;          ///< where it ends: the chart's rule, or whatever is next
    uint16_t grow;           ///< the headline's growth, in thousandths
    uint8_t labSz;           ///< the captions and the two group headings
    uint8_t heroSz, bigSz, headGap, slashW, subSz;
    int16_t heroY, subY;

    uint8_t gridNRows;       ///< 0 when the grid is empty or switched off
    uint8_t gridRows[6];     ///< cells on each row
    int16_t gridY;           ///< the first row's caption
    int16_t gridRowH;        ///< from one row's caption to the next one's
    uint8_t gridValSz;       ///< one size for every cell

    uint8_t clSize, clBoxed, clRuled, clRuledPad, clDated, clDateSz, clDateGap;
    uint16_t clGrow;         ///< the clock's growth, in thousandths
    int16_t clH;

    int16_t inRuleY, inLabY;
    int16_t inValY;          ///< the first field's top
    int16_t inVal2Y;         ///< the others' top, when they are on a line of their own
    uint8_t inValSz1, inValSz;
    uint16_t inW1Pm;         ///< the first field's share of the row, in thousandths
    bool    inStack;         ///< the first field on a line of its own

    int16_t sepH;            ///< the hairline between the columns

    // ── The sections under it ──
    bool    chart, forecast, week;
    int16_t rule2Y;          ///< the chart's rule; == rule3Y when there is no chart
    int16_t grY, grH;        ///< the chart image
    int16_t rule3Y;          ///< where the chart ends and the forecast (or the week) begins
    int16_t wkRuleY;         ///< the week strip's rule, or KDF_FOOT_Y without one
};

static inline int kdfMin(int a, int b) { return a < b ? a : b; }
static inline int kdfMax(int a, int b) { return a > b ? a : b; }
static inline int kdfScale(int v, int permille) { return (v * permille + 500) / 1000; }

/// Break `n` cells into `r` rows as evenly as they go, remainder to the
/// earlier rows. kdGridRowSplit() in KindleSlots.h is this with r fixed by the
/// three-across cap.
static inline void kdFlowSplit(int n, int r, uint8_t* rows) {
    const int base = n / r;
    int extra = n - base * r;
    for (int i = 0; i < r; i++) {
        rows[i] = (uint8_t)(base + (extra > 0 ? 1 : 0));
        if (extra > 0) extra--;
    }
}

/// The top block, laid out for a height of T.
/// The width the indoor row needs on one line with its first field at `s1`:
/// that field, and each of the others at six tenths of it or its caption,
/// whichever is wider.
static inline int kdFlowInNeed(int s1, int a1, const uint16_t* adv, int m) {
    int w = s1 * a1 / 1000 + KDF_CELL_PAD;
    const int s2 = s1 * 6 / 10;
    for (int i = 1; i < m; i++) {
        const int a = adv[i] ? adv[i] : 1000;
        w += kdfMax(s2 * a / 1000 + KDF_CELL_PAD, KDF_IN_CAP_W);
    }
    return w;
}

static inline void kdFlowTop(const KdFlowIn& in, int T, KdFlow& f) {
    f.topBot = (int16_t)(KDF_TOP_Y + T);
    int g = 1000 + (KDF_GROW_MAX - 1000) * (T - KDF_TOP_MIN) / (KDF_TOP_GROWN - KDF_TOP_MIN);
    g = kdfMax(1000, kdfMin(KDF_GROW_MAX, g));
    f.grow = (uint16_t)g;

    // ── The headline and the line under it ──
    f.labSz   = (uint8_t)(g >= KDF_GROW_BIG ? 15 : 14);
    f.heroSz  = (uint8_t)kdfScale(88, g);
    f.bigSz   = (uint8_t)kdfScale(44, kdfMin(g, KDF_GROW_BIG));
    f.headGap = (uint8_t)kdfScale(8, g);
    f.slashW  = (uint8_t)kdfScale(22, g);
    f.subSz   = (uint8_t)kdfScale(17, kdfMin(g, KDF_GROW_SUB));
    f.heroY   = (int16_t)KDF_HERO_Y;
    // The headline's growth, as air: the standalone page put 14 px more under
    // the headline and 6 more under the line below it than the ordinary one.
    const int air = g - 1000;
    f.subY    = (int16_t)(f.heroY + f.heroSz + 2 + air * 14 / (KDF_GROW_MAX - 1000));
    const int gridTop = in.sub
        ? f.subY + f.subSz + 13 + air * 6 / (KDF_GROW_MAX - 1000)
        : f.subY;
    const int bot = f.topBot - 8;

    // ── The grid ──
    f.gridNRows = 0;
    for (int i = 0; i < 6; i++) f.gridRows[i] = 0;
    f.gridY = (int16_t)gridTop;
    f.gridRowH = 0;
    f.gridValSz = 0;
    const int n = in.nGrid > 6 ? 6 : in.nGrid;
    if (n > 0) {
        const int areaH = bot - gridTop;
        const int cap   = f.heroSz * 60 / 100;
        const int rMin  = (n + 2) / 3;
        int best = -1, bestR = rMin;
        for (int r = rMin; r <= n; r++) {
            uint8_t rows[6];
            kdFlowSplit(n, r, rows);
            const int pitch = areaH / r;
            int v = pitch - f.labSz - 4 - KDF_GRID_GAP;
            int at = 0;
            for (int k = 0; k < r; k++) {
                const int cellW = KDF_COL_L / rows[k] - KDF_CELL_PAD;
                for (int c = 0; c < rows[k]; c++, at++) {
                    const int adv = in.gridAdv[at] ? in.gridAdv[at] : 1000;
                    v = kdfMin(v, cellW * 1000 / adv);
                }
            }
            v = kdfMin(v, cap);
            if (r == rMin) {
                // The ordinary page's sizes — three across at 27, fewer at 34 —
                // were measured in a browser against the widest each gets, so
                // this arrangement never sets smaller than that. The estimate
                // above is deliberately pessimistic, and without the floor it
                // would shrink a page that was known to fit.
                const int floorV = (rows[0] >= 3) ? 27 : 34;
                const int vh = pitch - f.labSz - 4 - KDF_GRID_GAP;
                v = kdfMax(v, kdfMin(floorV, vh));
            }
            if (v >= best) { best = v; bestR = r; }
        }
        f.gridNRows = (uint8_t)bestR;
        kdFlowSplit(n, bestR, f.gridRows);
        f.gridValSz = (uint8_t)kdfMax(best, 10);
        f.gridRowH  = (int16_t)(areaH / bestR);
        const int content = f.labSz + 4 + f.gridValSz;
        f.gridY = (int16_t)(gridTop + kdfMax(0, (f.gridRowH - content) / 2));
    }

    // ── The clock ──
    const int gc = kdfMin(g, KDF_GROW_CLOCK);
    f.clGrow     = (uint16_t)gc;
    f.clSize     = (uint8_t)(96 * gc / 1000);
    f.clBoxed    = (uint8_t)(84 * gc / 1000);
    f.clRuled    = (uint8_t)(72 * gc / 1000);
    f.clRuledPad = (uint8_t)(16 * gc / 1000);
    f.clDated    = (uint8_t)(66 * gc / 1000);
    f.clDateSz   = (uint8_t)(15 * gc / 1000);
    f.clDateGap  = (uint8_t)(6 * gc / 1000);
    f.clH        = (int16_t)(f.clSize + 1);

    // ── The indoor row ──
    f.inRuleY = (int16_t)(KDF_CL_Y + f.clH + 1);
    f.inLabY  = (int16_t)(f.inRuleY + 10 + air * 4 / (KDF_GROW_MAX - 1000));
    f.inValSz1 = f.inValSz = 0;
    f.inW1Pm = 1000;
    f.inStack = false;
    f.inValY = f.inVal2Y = (int16_t)(f.inLabY + f.labSz + 18);
    const int m = in.nIn > 3 ? 3 : in.nIn;
    if (m > 0) {
        const int top   = f.inLabY + f.labSz + 18;
        const int areaH = bot - top;
        const int cap   = f.heroSz * 80 / 100;
        const int a1    = in.inAdv[0] ? in.inAdv[0] : 1000;
        int others = 0;
        for (int i = 1; i < m; i++) others += in.inAdv[i] ? in.inAdv[i] : 1000;

        // All on one line, the first set larger and the others at six tenths
        // of it, hanging their captions above themselves.
        int s1;
        if (m == 1) s1 = (KDF_COL_R - KDF_CELL_PAD) * 1000 / a1;
        else        s1 = (KDF_COL_R - KDF_CELL_PAD * m) * 1000 / (a1 + others * 6 / 10);
        s1 = kdfMin(s1, areaH);
        s1 = kdfMin(s1, cap);
        // The ordinary page's 52, which was measured to fit.
        s1 = kdfMax(s1, kdfMin(52, areaH));
        // Each of the others also carries its caption above it, spaced out in
        // capitals, and that is wider than a short figure: the first field
        // gives up size until they all have room for theirs.
        while (m > 1 && s1 > 40 &&
               kdFlowInNeed(s1, a1, in.inAdv, m) > KDF_COL_R) s1--;

        // Or the first on a line of its own and the others under it.
        int s1s = 0;
        if (m >= 2) {
            s1s = (KDF_COL_R - KDF_CELL_PAD) * 1000 / a1;
            const int s2 = (KDF_COL_R - KDF_CELL_PAD * (m - 1)) * 1000 / others;
            s1s = kdfMin(s1s, s2 * 10 / 6);
            s1s = kdfMin(s1s, (areaH - 14 - f.labSz) * 10 / 16);
            s1s = kdfMin(s1s, cap);
        }

        if (s1s > s1 + 2) {
            f.inStack  = true;
            f.inValSz1 = (uint8_t)s1s;
            f.inValSz  = (uint8_t)(s1s * 6 / 10);
            const int h = f.inValSz1 + 10 + f.labSz + 4 + f.inValSz;
            f.inValY  = (int16_t)(top + kdfMax(0, (areaH - h) / 2));
            f.inVal2Y = (int16_t)(f.inValY + f.inValSz1 + 10 + f.labSz + 4);
        } else {
            f.inValSz1 = (uint8_t)s1;
            f.inValSz  = (uint8_t)(s1 * 6 / 10);
            f.inValY   = (int16_t)(top + kdfMax(0, (areaH - s1) / 2));
            f.inVal2Y  = (int16_t)(f.inValY + f.inValSz1 - f.inValSz);
            if (m > 1) {
                // The width each field needs, and the slack shared out in the
                // same proportion — so the first field gets what it takes to
                // be set larger, not a fixed 42 or 58 per cent.
                const int need1 = f.inValSz1 * a1 / 1000 + KDF_CELL_PAD;
                const int needO = kdFlowInNeed(f.inValSz1, a1, in.inAdv, m) - need1;
                f.inW1Pm = (uint16_t)(need1 * 1000 / kdfMax(1, need1 + needO));
            }
        }
    }

    f.sepH = (int16_t)(T - 10);
}

/// Whether two layouts set everything in the top block the same.
static inline bool kdFlowSameType(const KdFlow& a, const KdFlow& b) {
    if (a.heroSz != b.heroSz || a.clSize != b.clSize || a.labSz != b.labSz) return false;
    if (a.gridValSz != b.gridValSz || a.gridNRows != b.gridNRows) return false;
    if (a.inValSz1 != b.inValSz1 || a.inStack != b.inStack) return false;
    return true;
}

/// The whole page.
static inline KdFlow kdFlowCompute(const KdFlowIn& in) {
    KdFlow f;
    memset(&f, 0, sizeof(f));
    f.chart    = in.chart;
    f.forecast = in.forecast;
    f.week     = in.week;

    // Stacked from the bottom: the footer, the week strip, the forecast.
    f.wkRuleY = (int16_t)(in.week ? KDF_FOOT_Y - KDF_WEEK_H : KDF_FOOT_Y);
    const int below = f.wkRuleY - (in.forecast ? KDF_FC_H : 0);   // where the chart ends

    // What the top block and the chart have between them.
    const int avail = below - KDF_TOP_Y - (in.chart ? KDF_CHART_ABOVE + KDF_CHART_BELOW : 0);
    int T = in.chart ? avail - KDF_CHART_MIN : avail;
    if (T < KDF_TOP_MIN) T = KDF_TOP_MIN;      // cannot happen: everything on is exactly this

    kdFlowTop(in, T, f);
    if (in.chart) {
        // THE READINGS FIRST, THEN THE CHART: the smallest height at which the
        // top block is already setting everything as large as it ever will.
        // Anything past that is air in the readings and chart in the chart.
        KdFlow at;
        for (int t = KDF_TOP_MIN; t < T; t++) {
            kdFlowTop(in, t, at);
            if (kdFlowSameType(at, f)) { T = t; break; }
        }
        kdFlowTop(in, T, f);
        f.rule2Y = f.topBot;
        f.grY    = (int16_t)(f.rule2Y + KDF_CHART_ABOVE);
        f.grH    = (int16_t)(below - KDF_CHART_BELOW - f.grY);
        f.rule3Y = (int16_t)below;
    } else {
        f.rule2Y = f.rule3Y = f.topBot;
        f.grY = f.topBot;
        f.grH = 0;
    }
    return f;
}

// ---------------------------------------------------------------------------
// For the FBInk panel: the layout file's own names, at the panel's size
// ---------------------------------------------------------------------------
/// One value the panel lays over its layout file, as LY_<key>.
struct KdFlowKV { const char* key; int value; };

/// Scale a design pixel to a panel `resW` wide — the relation between
/// kindle/layout/600x800.conf and 1072x1448.conf.
static inline int kdFlowPanel(int v, unsigned resW) {
    return (int)((v * (long)resW + 300) / 600);
}

/// The panel's keys. Returns how many were written into `out`, which must hold
/// KDF_PANEL_KEYS. The names are the layout file's, so the reader applies them
/// by name and the file stays the description of everything that does not
/// move.
static const int KDF_PANEL_KEYS = 48;
static inline int kdFlowPanelKeys(const KdFlow& f, unsigned resW, KdFlowKV* out) {
    int n = 0;
    // Bounded: a key added below without KDF_PANEL_KEYS growing with it is
    // dropped (and fails test_panel_keys) rather than written past `out`.
    #define KDF_K(k, v)  do { if (n < KDF_PANEL_KEYS) { out[n].key = (k); \
                              out[n].value = kdFlowPanel((v), resW); } n++; } while (0)
    #define KDF_R(k, v)  do { if (n < KDF_PANEL_KEYS) { out[n].key = (k); \
                              out[n].value = (v); } n++; } while (0)
    KDF_K("GROUP_LAB_SZ", f.labSz);
    KDF_K("HERO_Y",       f.heroY);
    KDF_K("HERO_SZ",      f.heroSz);
    KDF_K("BIG_SZ",       f.bigSz);
    KDF_K("HEAD_GAP",     f.headGap);
    KDF_K("SLASH_W",      f.slashW);
    KDF_K("SUB_Y",        f.subY);
    KDF_K("SUB_SZ",       f.subSz);
    KDF_K("GRID_Y",       f.gridY);
    KDF_K("GRID_ROW_H",   f.gridRowH);
    KDF_K("GRID_LAB_SZ",  f.labSz);
    KDF_K("GRID_VAL_SZ",  f.gridValSz);
    KDF_K("GRID_VAL_SZ_3", f.gridValSz);
    KDF_K("SEP_H",        f.sepH);
    KDF_K("CL_SIZE",      f.clSize);
    KDF_K("CL_H",         f.clH);
    KDF_K("CL_SZ_BOXED",  f.clBoxed);
    KDF_K("CL_SZ_RULED",  f.clRuled);
    KDF_K("CL_RULED_PAD", f.clRuledPad);
    KDF_K("CL_SZ_DATED",  f.clDated);
    KDF_K("CL_DATE_SZ",   f.clDateSz);
    KDF_K("CL_DATE_GAP",  f.clDateGap);
    KDF_K("IN_RULE_Y",    f.inRuleY);
    KDF_K("IN_LAB_Y",     f.inLabY);
    KDF_K("IN_VAL_Y",     f.inValY);
    KDF_K("IN_VAL2_Y",    f.inVal2Y);
    KDF_K("IN_VAL_SZ",    f.inValSz);
    KDF_K("IN_VAL_SZ_1",  f.inValSz1);
    KDF_R("IN_W1",        f.inW1Pm);
    KDF_R("IN_STACK",     f.inStack ? 1 : 0);
    KDF_K("RULE2_Y",      f.rule2Y);
    KDF_K("LAB_CHART_Y",  f.rule2Y + 6);
    KDF_K("GR_Y",         f.grY);
    KDF_K("GR_H",         f.grH);
    KDF_K("KEY_Y",        f.grY + f.grH + 2);
    KDF_K("RULE3_Y",      f.rule3Y);
    KDF_K("LAB_FC_Y",     f.rule3Y + 6);
    KDF_K("FC_ICON_Y",    f.rule3Y + 28);
    KDF_K("FC_TEXT_Y",    f.rule3Y + 28);
    KDF_K("FC_TEMP_Y",    f.rule3Y + 62);
    KDF_K("FC_WIND_Y",    f.rule3Y + 100);
    KDF_K("OL0_Y",        f.rule3Y + 12);
    KDF_K("OL1_Y",        f.rule3Y + 12);
    KDF_K("OL2_Y",        f.rule3Y + 12);
    KDF_K("WK_HDG_RULE_Y", f.wkRuleY);
    KDF_K("WK_HDG_Y",     f.wkRuleY + 5);
    KDF_K("WK_Y",         f.wkRuleY + 24);
    KDF_R("FC_BAND",      f.forecast ? 1 : 0);
    #undef KDF_K
    #undef KDF_R
    return n > KDF_PANEL_KEYS ? -1 : n;      // -1: the table outgrew its count
}

/// The grid's rows, as the panel reads them: "2", "1 1", "3 2".
static inline void kdFlowRowsText(const KdFlow& f, char* buf, size_t len) {
    size_t at = 0;
    if (len == 0) return;
    buf[0] = '\0';
    for (int r = 0; r < f.gridNRows && at + 3 < len; r++) {
        if (r) buf[at++] = ' ';
        buf[at++] = (char)('0' + f.gridRows[r]);
    }
    buf[at] = '\0';
}

// ---------------------------------------------------------------------------
// For the browser page: overrides after the design's own sheet
// ---------------------------------------------------------------------------
/// What the browser page spends, in design pixels, over what the panel does on
/// the same sections — measured in a browser with every section on (777 px of
/// page against the panel's 800, top block at its 262): the chart section's
/// caption line, key and rule margins take 9 more than FBInk's, the forecast
/// band 14, the week strip 7, the footer and the body's margin 6 more than the
/// top block gives back. Plus a few pixels for the reader's own fonts, which
/// are not the ones the measurement ran on. This comes out of the chart, which
/// is happy to be any height, or out of the top block when there is no chart.
static const int KDF_HTML_FOOT   = 6;
static const int KDF_HTML_CHART  = 9;
static const int KDF_HTML_FC     = 14;
static const int KDF_HTML_WEEK   = 7;
static const int KDF_HTML_SPARE  = 14;

static inline int kdFlowHtmlExtra(const KdFlow& f) {
    return KDF_HTML_FOOT + KDF_HTML_SPARE + (f.chart ? KDF_HTML_CHART : 0) +
           (f.forecast ? KDF_HTML_FC : 0) + (f.week ? KDF_HTML_WEEK : 0);
}

/// The layout the browser page draws. With a chart it is the panel's, less
/// kdFlowHtmlExtra() off the chart; without one the top block is laid out
/// again that much shorter, so what is in it is sized for the room it gets.
static inline KdFlow kdFlowComputeHtml(const KdFlowIn& in) {
    KdFlow f = kdFlowCompute(in);
    if (!f.chart) kdFlowTop(in, f.topBot - KDF_TOP_Y - kdFlowHtmlExtra(f), f);
    return f;
}

/// The height of the top block's two cells on the browser page.
static inline int kdFlowHtmlColH(const KdFlow& f) {
    return f.topBot - KDF_TOP_Y - 12;
}

/// The page's CSS for this layout, appended after kdSkinCss() so it wins over
/// both the design's sheet and the clock style's own sizes. `px` scales a
/// design pixel to the page — kdPx() in the firmware.
///
/// Every rule here is a size or a height; the design's greys, faces and
/// spacing are left where the sheet put them.
template <typename StringT, typename PxFn>
inline void kdFlowCss(StringT& out, const KdFlow& f, uint8_t clockStyle, PxFn px) {
    #define KDF_PX(v) out += (int)px(v); out += "px"
    out += ".lab{font-size:";        KDF_PX(f.labSz);    out += "}";
    out += ".v1{font-size:";         KDF_PX(f.heroSz);   out += "}";
    out += ".v2,.slash{font-size:";  KDF_PX(f.bigSz);    out += "}";
    // The slash's padding and lift grow with the headline it sits beside.
    out += ".slash{padding:0 ";      KDF_PX(f.headGap - 1);
    out += ";top:";                  KDF_PX(-kdfScale(5, f.grow)); out += "}";
    out += ".sub{font-size:";        KDF_PX(f.subSz);    out += "}";
    // The top block is as tall as the layout gave it, whatever is in it: its
    // two cells carry the height, and the grid's rows share theirs out.
    out += ".col-l,.col-r{height:";  KDF_PX(kdFlowHtmlColH(f)); out += "}";
    if (f.gridNRows) {
        out += ".gv,.grid-3 .gv{font-size:"; KDF_PX(f.gridValSz); out += "}";
        out += ".grid{margin-top:0}.grid td{vertical-align:middle;height:";
        KDF_PX(f.gridRowH - 1); out += "}";
    }
    if (f.inValSz1) {
        out += ".iv{font-size:";   KDF_PX(f.inValSz);  out += "}";
        out += ".iv-1{font-size:"; KDF_PX(f.inValSz1); out += "}";
        // Down to where the layout put the first field: centred in what the
        // column has left under the clock, not hard against its heading.
        out += ".inrow{margin-top:";
        KDF_PX(8 + f.inValY - (f.inLabY + f.labSz + 18)); out += "}";
        out += ".inrow2{margin-top:"; KDF_PX(10); out += "}";
    }
    // The clock, in whichever style it is set — the same arms kdSkinCss()
    // writes, at this layout's size.
    const int gc = f.clGrow;
    switch (clockStyle) {
        case 1:   // boxed
            out += ".clock{height:";      KDF_PX(96 * gc / 1000);
            out += ";line-height:";       KDF_PX(96 * gc / 1000);
            out += ";font-size:";         KDF_PX(f.clBoxed); out += "}";
            break;
        case 2:   // ruled
            out += ".clock{font-size:";   KDF_PX(f.clRuled);
            out += ";line-height:";       KDF_PX(84 * gc / 1000);
            out += ";padding-top:";       KDF_PX(f.clRuledPad); out += "}";
            break;
        case 3:   // dated
            out += ".clock{height:";      KDF_PX(76 * gc / 1000);
            out += ";line-height:";       KDF_PX(76 * gc / 1000);
            out += ";font-size:";         KDF_PX(f.clDated);
            out += "}.clock-d{height:";   KDF_PX(24 * gc / 1000);
            out += ";font-size:";         KDF_PX(f.clDateSz); out += "}";
            break;
        default:
            out += ".clock{font-size:";   KDF_PX(f.clSize);
            out += ";line-height:";       KDF_PX(100 * gc / 1000); out += "}";
            break;
    }
    // The line printed in the clock's place when there is no time to show:
    // as tall as the clock it stands in for, so the indoor row stays where
    // the layout put it.
    out += ".clock-x{font-size:";   KDF_PX(f.bigSz);
    out += ";line-height:";         KDF_PX(100 * gc / 1000); out += "}";
    #undef KDF_PX
}

/// The chart's height on the browser page, in design pixels.
static inline int kdFlowHtmlChartH(const KdFlow& f) {
    return f.chart ? f.grH - kdFlowHtmlExtra(f) : 0;
}
