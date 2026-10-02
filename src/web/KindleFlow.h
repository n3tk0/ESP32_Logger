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
//      fixed percentages, and keeps all of them on one line.
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

/// A figure's width when a reading is SIZED, in thousandths of the type size.
static const unsigned KDF_FIG_SIZE = 620;

/// kdAdvanceMille() with each figure at KDF_FIG_SIZE: what a large value is
/// sized by, and where the panel puts the unit after it.
///
/// The 0.5 kdAdvanceMille() gives a figure is too narrow for both. The
/// page's fallback, Georgia, has figures of about 0.62 em, and the panel's
/// Bookerly is wider than 0.5 too. With the readings as large as their column
/// allows, a pressure of 1013 was cut at its right edge, and the panel set
/// "hPa" onto its last figure.
static inline unsigned kdFigAdvance(const char* text) {
    unsigned figs = 0;
    for (const char* c = text ? text : ""; *c; c++) if (*c >= '0' && *c <= '9') figs++;
    return kdAdvanceMille(text) + figs * (KDF_FIG_SIZE - 500u);
}

/// How wide a value comes out with its unit and arrow, in thousandths of the
/// value's type size — kdFlowWorstAdvance() without the widening, for the
/// headline, which is sized to what it prints (see kdFlowHeadFit()).
static inline unsigned kdFlowFieldAdvance(const char* text, const char* unit, bool arrow) {
    unsigned adv = kdFigAdvance(text);
    if (unit && *unit) {
        if (!strcmp(unit, "\xC2\xB0"))   adv += 330u * 34u / 100u;      // the degree
        else if (!strcmp(unit, "%"))     adv += 800u * 42u / 100u;
        else                             adv += (250u + kdAdvanceMille(unit)) * 42u / 100u;
    }
    if (arrow) adv += 350u;
    return adv;
}

/// The widest a place's value, unit and arrow come out, in thousandths of the
/// VALUE's type size — the three are drawn at three sizes, and this is the
/// sum in the value's terms: the unit at 0.42 of it (the degree at 0.34), the
/// tendency arrow at 0.5 with its 0.15 em of padding.
///
/// `text` is what the place prints now, already at its decimals.
static inline unsigned kdFlowWorstAdvance(const char* metric, const char* text,
                                          const char* unit, bool arrow,
                                          bool assumeSign = true) {
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
    // assumeSign false: no sign reserved, not even the reading's own — the
    // caller measures what is printed separately (kdFlowFirstInAdvance()).
    if (assumeSign && (neg || kdFlowSigned(metric))) worst[at++] = '-';
    for (int i = 0; i < need && at < sizeof(worst) - 1; i++) worst[at++] = '0';
    // Whatever follows the integer part — the decimals — as it is printed.
    for (; *q && at < sizeof(worst) - 1; q++) worst[at++] = *q;
    worst[at] = '\0';

    return kdFlowFieldAdvance(worst, unit, arrow);
}

/// What the value beside the headline is SIZED by: two figures, a sign where
/// the metric can go below zero, and the decimals as printed. "37%" and "5%"
/// are one size; "100%", which fog does give, is wider and makes only this
/// value smaller (see kdFlowHeadFit()). Not kdFlowWorstAdvance(): it keeps
/// room for three figures of a humidity, and the headline would pay for it.
static inline unsigned kdFlowPairAdvance(const char* metric, const char* text,
                                         const char* unit, bool arrow) {
    char sized[24];
    size_t at = 0;
    const char* p = text ? text : "";
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); p++; }
    while (*p >= '0' && *p <= '9') p++;
    if (neg || kdFlowSigned(metric)) sized[at++] = '-';
    sized[at++] = '0';
    sized[at++] = '0';
    for (; *p && at < sizeof(sized) - 1; p++) sized[at++] = *p;
    sized[at] = '\0';
    return kdFlowFieldAdvance(sized, unit, arrow);
}

/// What the indoor row's FIRST place is sized by — every producer of
/// KdFlowIn::inAdv[0] calls this, and the settings page's copy of it.
///
/// An indoor air temperature keeps room for two digits and its decimals, but
/// not for the minus sign every other temperature on the page is given: a
/// room does not reach it, and the reservation cost this field a sixth of its
/// size. NOT by the reading's sign, though — that would re-lay the whole row
/// every time an unheated room crossed zero. The printed text counts only
/// when it is wider than the reservation, which a reading does below -9.9.
///
/// Any other metric in that place — a dew point, which a heated room in
/// winter does take below zero — is measured as everywhere else.
static inline unsigned kdFlowFirstInAdvance(const char* metric, const char* text,
                                            const char* unit, bool arrow) {
    if (!metric || strcmp(metric, "temperature") != 0)
        return kdFlowWorstAdvance(metric, text, unit, arrow);
    const unsigned worst = kdFlowWorstAdvance(metric, text, unit, arrow, false);
    const unsigned now   = kdFlowFieldAdvance(text ? text : "", unit, arrow);
    return now > worst ? now : worst;
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
static const int KDF_GROW_BIG    = 1090;  ///< the value beside it: 52 -> 57
/// The value beside the headline, on the ordinary page. It was 44, the layout
/// file's BIG_SZ, and read as a footnote to the headline from across a room;
/// at 52 the headline gives up about five pixels for it ("21.7°" 76 -> 71).
static const int KDF_BIG_SZ      = 52;
static const int KDF_GROW_SUB    = 1120;  ///< the line under it: 17 -> 19

// ---------------------------------------------------------------------------
// The layout
// ---------------------------------------------------------------------------
/// What the layout is worked out from.
struct KdFlowIn {
    bool    chart    = true;    ///< KSHOW_CHART
    bool    forecast = true;    ///< a forecast band is drawn (not standalone)
    bool    week     = true;    ///< KSHOW_WEEK
    bool    clock    = true;    ///< KSHOW_CLOCK
    bool    land     = false;   ///< the landscape page: KROT_90 or KROT_270
    bool    sub      = true;    ///< the line under the headline has something in it
    uint8_t nGrid    = 0;       ///< grid places with a reading, 0..6
    uint16_t gridAdv[6] = {0, 0, 0, 0, 0, 0};   ///< kdFlowWorstAdvance() of each
    uint8_t nIn      = 0;       ///< indoor places with a reading, 0..3
    uint16_t inAdv[3]   = {0, 0, 0};
    uint8_t outPct   = 100;     ///< the grid's values, per cent of the most that fits
    uint8_t inPct    = 100;     ///< the indoor values, likewise
    uint16_t heroAdv = 0;       ///< the headline's kdFlowWorstAdvance(); 0 for none
    uint16_t bigAdv  = 0;       ///< the value beside it as printed; 0 when there is none
    uint16_t bigFitAdv = 0;     ///< ...and its kdFlowPairAdvance(); 0: bigAdv
    /// Whoever draws it knows the two-column indoor row. An FBInk script from
    /// before it would draw the column's two readings side by side in the
    /// column's width, one over the other; it says ?col=1 when it knows.
    bool    inColOk  = true;
    /// The wall page — KPAGE_WALL, upright, and a renderer that knows it: the
    /// browser page always, an FBInk script when it says ?wall=1. See
    /// kdFlowWall().
    bool    wall     = false;
};

/// Outlook columns at most: five on the landscape page, three upright.
static const int KDF_OL_MAX = 5;

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
    int16_t inVal2Y;         ///< the others' top; in a column, the upper one's
    int16_t inVal3Y;         ///< in a column: the lower one's top, on the first one's bottom line
    uint8_t inValSz1, inValSz;
    uint16_t inW1Pm;         ///< the first field's share of the row, in thousandths
    bool    inStack;         ///< the first field on a line of its own (never, now)
    bool    inCol;           ///< the other two one above the other, beside the first

    int16_t sepH;            ///< the hairline between the columns

    // ── The sections under it ──
    bool    chart, forecast, week, clock;
    int16_t rule2Y;          ///< the chart's rule; == rule3Y when there is no chart
    int16_t grY, grH;        ///< the chart image
    int16_t rule3Y;          ///< where the chart ends and the forecast (or the week) begins
    int16_t wkRuleY;         ///< the week strip's rule, or KDF_FOOT_Y without one

    // ── Across ──
    // Everything above is a height, and on the upright page that is all that
    // moves: every x is the layout file's. The landscape page moves things
    // sideways too — the chart beside the readings, the week strip beside the
    // clock — so these carry the x of everything, on both pages; on the
    // upright one they are exactly the file's numbers.
    bool    land;            ///< the landscape page, 800 x 600
    int16_t pageW, pageH;    ///< 600 x 800, or 800 x 600
    int16_t groupY;          ///< the outdoor heading (the file's TOP_Y)
    int16_t colLX, colLW;    ///< the outdoor column
    int16_t inX, inW;        ///< the indoor row's column
    int16_t sepX, sepY;      ///< the hairline between the columns, from its top
    int16_t clX, clY, clW;   ///< the clock's rectangle
    int16_t topRowY;         ///< landscape: the rule under the clock and the week; 0 upright
    int16_t rule2X, rule2W;  ///< the rule over the chart; 0 wide when there is none
    int16_t labChartX;
    int16_t grX, grW;        ///< the chart image
    int16_t keyInX;          ///< where the key's indoor entry starts
    bool    keyBand;         ///< whether the key has room for "shaded band = ..."
    uint8_t olN;             ///< outlook columns in the forecast band: 3, or 5
    int16_t olX[KDF_OL_MAX];
    int16_t wkX, wkY, wkCellW, wkHdgY;
    bool    wkRule;          ///< whether the week strip has a hairline of its own
    int16_t footY;           ///< the footer's rule
    int16_t statX;           ///< where the footer's status line starts
    int16_t battX, battY;    ///< the low-battery badge

    // ── The wall page ──
    // On the other two pages these are what the layout file already says:
    // the headline centred in its column, no rule under it, the forecast's
    // current conditions at the band's left with the outlook beside them.
    bool    wall;            ///< kdFlowWall()
    int16_t headW;           ///< what a lone headline is centred in
    int16_t headRuleY;       ///< the rule under the headline's row; 0 for none
    bool    inVcol;          ///< the indoor readings one under the other
    int16_t sep2X, sep2Y, sep2H;   ///< the hairline between the clock and the forecast
    int16_t labFcX, labFcY;  ///< the forecast's heading
    int16_t fcIconX, fcIconY;
    int16_t fcTextX, fcTextY, fcTextW;   ///< the summary; fcTextW 0: not fitted
    int16_t fcTempX, fcTempY, fcWindX, fcWindY;
    uint8_t fcTextSz, fcTempSz, fcWindSz;
};

static inline int kdfMin(int a, int b) { return a < b ? a : b; }
static inline int kdfMax(int a, int b) { return a > b ? a : b; }
static inline int kdfScale(int v, int permille) { return (v * permille + 500) / 1000; }
/// A size setting's per cent; 0 is the most that fits, as 100 is.
static inline int kdfPct(uint8_t p) { return p ? p : 100; }

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

/// The headline's sizes at a growth of `g` thousandths. Below 1000 only on the
/// landscape page, where the readings share their height with the indoor row.
static inline void kdFlowType(int g, KdFlow& f) {
    f.grow    = (uint16_t)g;
    f.labSz   = (uint8_t)(g >= KDF_GROW_BIG ? 15 : 14);
    f.heroSz  = (uint8_t)kdfScale(88, g);
    f.bigSz   = (uint8_t)kdfScale(KDF_BIG_SZ, kdfMin(g, KDF_GROW_BIG));
    f.headGap = (uint8_t)kdfScale(8, g);
    f.slashW  = (uint8_t)kdfScale(22, g);
    f.subSz   = (uint8_t)kdfScale(17, kdfMin(g, KDF_GROW_SUB));
}

/// The clock at a growth of `gc` thousandths.
static inline void kdFlowClockSizes(int gc, KdFlow& f) {
    f.clGrow     = (uint16_t)gc;
    f.clSize     = (uint8_t)(96 * gc / 1000);
    f.clBoxed    = (uint8_t)(84 * gc / 1000);
    f.clRuled    = (uint8_t)(72 * gc / 1000);
    f.clRuledPad = (uint8_t)(16 * gc / 1000);
    f.clDated    = (uint8_t)(66 * gc / 1000);
    f.clDateSz   = (uint8_t)(15 * gc / 1000);
    f.clDateGap  = (uint8_t)(6 * gc / 1000);
    f.clH        = (int16_t)(f.clSize + 1);
}

/// The headline made to fit its column. It grew with the room above and
/// below it and nothing looked across: "23.5° / 1013 hPa" at 88 and 44 is
/// wider than the 270 px the column has, and the pressure ran off its right
/// edge.
///
/// THE VALUE BESIDE THE HEADLINE KEEPS ITS SIZE; THE HEADLINE GIVES WAY.
/// Sized by what they printed with the second one giving way first,
/// "21.7° / 37%" took the humidity down to 28 and it could not be read. Now
/// the value beside it is sized for two figures (kdFlowPairAdvance(), "00%"),
/// so "5%" and "37%" are one size, and keeps its KDF_BIG_SZ; the headline,
/// by what it prints, takes what is left, down to KDF_HERO_MIN: 71 for
/// "21.7°", 62 for "-38.8°". Only a value wider than it was sized for —
/// "100%" — then makes the two give way in turn.
static const int KDF_BIG_MIN  = 28;
static const int KDF_HERO_MIN = 40;
static inline void kdFlowHeadFit(const KdFlowIn& in, KdFlow& f) {
    if (!in.heroAdv) return;
    const int heroA = in.heroAdv;
    const int bigA  = in.bigAdv;
    const int bigS  = bigA ? (in.bigFitAdv ? in.bigFitAdv : bigA) : 0;
    int hero = f.heroSz, big = f.bigSz;
    const int room = f.colLW - (bigA ? f.headGap + f.slashW : 0);
    while (hero > KDF_HERO_MIN && hero * heroA / 1000 + big * bigS / 1000 > room) hero--;
    while (bigA && big > KDF_BIG_MIN && hero * heroA / 1000 + big * bigS / 1000 > room) big--;
    // Wider than it was sized for ("100%"), or still too wide: the two give
    // way in turn, so neither is the only one to pay.
    bool turn = false;
    while (hero * heroA / 1000 + (bigA ? big * kdfMax(bigA, bigS) / 1000 : 0) > room) {
        turn = !turn;
        if (bigA && big > KDF_BIG_MIN && (turn || hero <= KDF_HERO_MIN)) big--;
        else if (hero > KDF_HERO_MIN) hero--;
        else if (bigA && big > KDF_BIG_MIN) big--;
        else break;
    }
    f.heroSz = (uint8_t)hero;
    f.bigSz  = (uint8_t)big;
}

/// The grid between `gridTop` and `bot`, f.colLW across: every way of breaking
/// its readings into rows tried, and the one that sets them largest kept.
///
/// `only`, when set, is the one number of rows to use rather than a search:
/// the wall page's grid is laid out by how many readings it has, not by what
/// sets them largest.
static inline void kdFlowGrid(const KdFlowIn& in, int gridTop, int bot, KdFlow& f,
                              int only = 0) {
    f.gridNRows = 0;
    for (int i = 0; i < 6; i++) f.gridRows[i] = 0;
    f.gridY = (int16_t)gridTop;
    f.gridRowH = 0;
    f.gridValSz = 0;
    const int n = in.nGrid > 6 ? 6 : in.nGrid;
    if (n > 0) {
        const int areaH = bot - gridTop;
        // As large as the room allows, but never larger than the headline:
        // one reading alone used to stop at six tenths of it, and left the
        // rest of its row white.
        const int cap   = f.heroSz;
        const int rMin  = (n + 2) / 3;
        int best = -1, bestR = only ? only : rMin;
        for (int r = only ? only : rMin; r <= (only ? only : n); r++) {
            uint8_t rows[6];
            kdFlowSplit(n, r, rows);
            const int pitch = areaH / r;
            int v = pitch - f.labSz - 4 - KDF_GRID_GAP;
            int at = 0;
            for (int k = 0; k < r; k++) {
                const int cellW = f.colLW / rows[k] - KDF_CELL_PAD;
                for (int c = 0; c < rows[k]; c++, at++) {
                    const int adv = in.gridAdv[at] ? in.gridAdv[at] : 1000;
                    v = kdfMin(v, cellW * 1000 / adv);
                }
            }
            v = kdfMin(v, cap);
            if (r == rMin && !only) {
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
        f.gridValSz = (uint8_t)kdfMax(best * kdfPct(in.outPct) / 100, 10);
        f.gridRowH  = (int16_t)(areaH / bestR);
        const int content = f.labSz + 4 + f.gridValSz;
        f.gridY = (int16_t)(gridTop + kdfMax(0, (f.gridRowH - content) / 2));
    }
}

/// The outdoor column under f.heroY: the line under the headline and the grid,
/// which ends at `bot`. Sized to f.colLW across.
static inline void kdFlowOutdoor(const KdFlowIn& in, int bot, KdFlow& f) {
    kdFlowHeadFit(in, f);
    // The headline's growth, as air: the standalone page put 14 px more under
    // the headline and 6 more under the line below it than the ordinary one.
    const int air = kdfMax(0, f.grow - 1000);
    f.subY    = (int16_t)(f.heroY + f.heroSz + 2 + air * 14 / (KDF_GROW_MAX - 1000));
    const int gridTop = in.sub
        ? f.subY + f.subSz + 13 + air * 6 / (KDF_GROW_MAX - 1000)
        : f.subY;

    kdFlowGrid(in, gridTop, bot, f);
}

/// The indoor row under f.inRuleY, ending at `bot`, f.inW across.
///
/// NEVER ONE FIELD OVER THE OTHERS. The first field used to take a line of its
/// own whenever that set it larger, which with three readings was nearly
/// always, so f.inStack is always false and IN_STACK still goes out as 0.
///
/// One field stands alone, as large as the room and the headline allow. Two
/// share a line, the second at six tenths of the first and on its bottom
/// edge. Three, upright, are two columns: the first large on the left, and
/// the other two one above the other beside it (f.inCol), the upper one
/// starting under the heading and the lower one on the first one's bottom
/// line. The landscape row is too short for that, and keeps all three on one
/// line.
static inline void kdFlowIndoor(const KdFlowIn& in, int bot, KdFlow& f) {
    const int air = kdfMax(0, f.grow - 1000);
    // The narrower of the two renderers' widths — see KDF_COL_R.
    const int W = f.inW - 12;
    f.inLabY  = (int16_t)(f.inRuleY + 10 + air * 4 / (KDF_GROW_MAX - 1000));
    f.inValSz1 = f.inValSz = 0;
    f.inW1Pm = 1000;
    f.inStack = false;
    f.inCol = false;
    f.inValY = f.inVal2Y = f.inVal3Y = (int16_t)(f.inLabY + f.labSz + 18);
    const int m = in.nIn > 3 ? 3 : in.nIn;
    if (m == 0) return;
    const int top   = f.inLabY + f.labSz + 18;
    const int areaH = bot - top;
    // Never larger than the headline; it used to stop at eight tenths of it.
    // The headline's size before kdFlowHeadFit(): a wide second value that
    // shrinks it has nothing to do with this column.
    const int cap   = kdfScale(88, f.grow);
    const int a1    = in.inAdv[0] ? in.inAdv[0] : 1000;
    const int pct   = kdfPct(in.inPct);

    // The column beside the first field: as wide as the wider of the two, or
    // of their captions, and two captions and two values tall. When nothing
    // down to 20 fits, the row stays on one line.
    const int colTop = f.inLabY + f.labSz + 6;
    const int a2 = kdfMax(in.inAdv[1] ? in.inAdv[1] : 1000,
                          in.inAdv[2] ? in.inAdv[2] : 1000);
    //
    // THE TWO BESIDE IT GIVE WAY FIRST. They were held at six tenths of the
    // first, so the first could only grow as far as their column let it, and
    // stopped well short of the room it stands in — the headline across the
    // rule fills its line, and this did not. They may now go down to half of
    // it, and do only when that is what lets the first one grow.
    //
    // Not a search over both: this runs for every height kdFlowComputeAt()
    // tries, on every render. For a first field at t, the side size is the
    // largest the width and the column's height allow, at most six tenths of
    // t — worked out, then checked, and stepped down past a rounding edge.
    int c1 = 0, cs = 0;
    if (m == 3 && !f.land && in.inColOk) {
        const int room = bot - colTop;
        const int sH   = (room - 6) / 2 - f.labSz - 4;       // the stack's height
        for (int t = kdfMin(cap, areaH); t >= 20 && !c1; t--) {
            const int w1 = t * a1 / 1000 + KDF_CELL_PAD;
            if (w1 + KDF_IN_CAP_W > W) continue;
            const int sW = ((W - w1 - KDF_CELL_PAD + 1) * 1000 - 1) / a2;   // its width
            int s = kdfMin(t * 6 / 10, kdfMin(sH, sW));
            while (s >= t / 2 &&
                   !(w1 + kdfMax(s * a2 / 1000 + KDF_CELL_PAD, KDF_IN_CAP_W) <= W &&
                     2 * (f.labSz + 4 + s) + 6 <= room))
                s--;
            if (s >= t / 2) { c1 = t; cs = s; }
        }
    }
    if (c1) {
        const int s1 = kdfMax(20, c1 * pct / 100);
        const int s  = cs * s1 / c1;
        const int wR = kdfMax(s * a2 / 1000 + KDF_CELL_PAD, KDF_IN_CAP_W);
        const int stackH = 2 * (f.labSz + 4 + s) + 6;
        f.inCol    = true;
        f.inValSz1 = (uint8_t)s1;
        f.inValSz  = (uint8_t)s;
        // The column is the taller of the two, so it is what gets centred in
        // the room, and the first field stands on its bottom line.
        const int base = colTop + stackH + kdfMax(0, (bot - colTop - stackH) / 2);
        f.inValY  = (int16_t)(base - s1);
        f.inVal2Y = (int16_t)(base - stackH + f.labSz + 4);
        f.inVal3Y = (int16_t)(base - s);
        const int need1 = s1 * a1 / 1000 + KDF_CELL_PAD;
        f.inW1Pm = (uint16_t)(need1 * 1000 / kdfMax(1, need1 + wR));
        return;
    }

    int others = 0;
    for (int i = 1; i < m; i++) others += in.inAdv[i] ? in.inAdv[i] : 1000;
    // On one line, the first set larger and the others at six tenths of it,
    // hanging their captions above themselves.
    int s1;
    if (m == 1) s1 = (W - KDF_CELL_PAD) * 1000 / a1;
    else        s1 = (W - KDF_CELL_PAD * m) * 1000 / (a1 + others * 6 / 10);
    s1 = kdfMin(s1, areaH);
    s1 = kdfMin(s1, cap);
    // The ordinary page's 52, which was measured to fit.
    s1 = kdfMax(s1, kdfMin(52, areaH));
    // Each of the others also carries its caption above it, spaced out in
    // capitals, and that is wider than a short figure: the first field
    // gives up size until they all have room for theirs.
    while (m > 1 && s1 > 30 &&
           kdFlowInNeed(s1, a1, in.inAdv, m) > W) s1--;
    s1 = kdfMax(20, s1 * pct / 100);

    f.inValSz1 = (uint8_t)s1;
    f.inValSz  = (uint8_t)(s1 * 6 / 10);
    f.inValY   = (int16_t)(top + kdfMax(0, (areaH - s1) / 2));
    f.inVal2Y  = f.inVal3Y = (int16_t)(f.inValY + f.inValSz1 - f.inValSz);
    if (m > 1) {
        // The width each field needs, and the slack shared out in the
        // same proportion — so the first field gets what it takes to
        // be set larger, not a fixed 42 or 58 per cent.
        const int need1 = f.inValSz1 * a1 / 1000 + KDF_CELL_PAD;
        const int needO = kdFlowInNeed(f.inValSz1, a1, in.inAdv, m) - need1;
        f.inW1Pm = (uint16_t)(need1 * 1000 / kdfMax(1, need1 + needO));
    }
}

/// The upright top block, laid out for a height of T.
static inline void kdFlowTop(const KdFlowIn& in, int T, KdFlow& f) {
    f.topBot = (int16_t)(KDF_TOP_Y + T);
    int g = 1000 + (KDF_GROW_MAX - 1000) * (T - KDF_TOP_MIN) / (KDF_TOP_GROWN - KDF_TOP_MIN);
    g = kdfMax(1000, kdfMin(KDF_GROW_MAX, g));
    kdFlowType(g, f);
    f.heroY = (int16_t)KDF_HERO_Y;
    const int bot = f.topBot - 8;
    kdFlowOutdoor(in, bot, f);
    kdFlowClockSizes(kdfMin(g, KDF_GROW_CLOCK), f);
    // Without the clock the indoor row moves up to the outdoor heading's line.
    f.inRuleY = (int16_t)(in.clock ? KDF_CL_Y + f.clH + 1
                                   : KDF_TOP_Y - 10 - kdfMax(0, g - 1000) * 4 / (KDF_GROW_MAX - 1000));
    kdFlowIndoor(in, bot, f);
    // Nothing in the right column: the outdoor one has the page's width, and
    // there is nothing to separate it from.
    f.sepH = (int16_t)(f.colLW > KDF_COL_L ? 0 : T - 10);
}

/// Whether two layouts set everything in the top block the same.
static inline bool kdFlowSameType(const KdFlow& a, const KdFlow& b) {
    if (a.heroSz != b.heroSz || a.bigSz != b.bigSz || a.clSize != b.clSize || a.labSz != b.labSz) return false;
    if (a.gridValSz != b.gridValSz || a.gridNRows != b.gridNRows) return false;
    if (a.inValSz1 != b.inValSz1 || a.inStack != b.inStack || a.inCol != b.inCol) return false;
    return true;
}

// ---------------------------------------------------------------------------
// The landscape page, 800 x 600
// ---------------------------------------------------------------------------
// The upright page's sections turned into rows and columns:
//
//   clock          | the week strip, seven days across         (the top row)
//   outdoor        | the chart, the height of the middle band
//   indoor, 1 row  |
//   forecast now   | five outlook columns                       (the band)
//   the footer
//
// Heights are the upright page's where they are the same thing (the band 124,
// the footer 36); what is left between the top row and the band is split
// across, not stacked, so here the readings do not trade height with the chart
// — they trade it with the indoor row, and their size is the most that fits.
static const int KDF_LAND_W      = 800;
static const int KDF_LAND_H      = 600;
static const int KDF_LAND_FOOT_Y = 564;   ///< the footer: 36 px, as upright
static const int KDF_LAND_X1     = 782;   ///< the right margin
static const int KDF_LAND_COL    = 300;   ///< the readings' column beside the chart
static const int KDF_LAND_SEP    = 328;
static const int KDF_LAND_CLOCK  = 840;   ///< the clock's growth: 96 -> 80
static const int KDF_LAND_ROW_W  = 77;    ///< the week strip's own height
static const int KDF_IN_H        = 102;   ///< the indoor row, rule to rule
static const int KDF_LAND_GROW_MIN = 640;
static const int KDF_OL_PITCH    = 92;    ///< one outlook column to the next

/// The landscape page with its footer at `footY` — KDF_LAND_FOOT_Y for the
/// panel, less the browser's extra for the page.
static inline void kdFlowLand(const KdFlowIn& in, int footY, KdFlow& f) {
    const int X0 = 18, X1 = KDF_LAND_X1;
    f.land  = true;
    f.pageW = (int16_t)KDF_LAND_W;
    f.pageH = (int16_t)KDF_LAND_H;
    f.footY = (int16_t)footY;
    f.statX = (int16_t)(KDF_LAND_W - 204);

    // ── The top row: the clock, and the week strip beside it ──
    kdFlowClockSizes(KDF_LAND_CLOCK, f);
    const bool row  = in.clock || in.week;
    const int rowH  = in.clock ? f.clH + 7 : KDF_LAND_ROW_W + 4;
    const int rowBot = KDF_TOP_Y + rowH;
    f.topRowY = (int16_t)(row ? rowBot : 0);
    f.clY     = (int16_t)(KDF_TOP_Y + 4);
    f.clW     = (int16_t)KDF_COL_R + 12;
    // Alone, the clock stands in the middle of the row.
    f.clX     = (int16_t)(in.week ? X0 : (KDF_LAND_W - f.clW) / 2);
    f.wkX     = (int16_t)(in.clock ? X0 + KDF_LAND_COL : X0);
    f.wkCellW = (int16_t)((X1 - f.wkX) / 7);
    f.wkHdgY  = (int16_t)(KDF_TOP_Y + (rowH - KDF_LAND_ROW_W) / 2);
    f.wkY     = (int16_t)(f.wkHdgY + 19);
    f.wkRule  = false;
    f.wkRuleY = f.footY;

    // ── The middle band ──
    const int midTop = row ? rowBot + 8 : KDF_TOP_Y;
    f.rule3Y = (int16_t)(in.forecast ? footY - KDF_FC_H : footY);
    const int midBot = f.rule3Y;
    f.topBot = (int16_t)midBot;
    f.colLX  = (int16_t)X0;
    f.colLW  = (int16_t)(in.chart ? KDF_LAND_COL : X1 - X0);
    f.sepX   = (int16_t)KDF_LAND_SEP;
    f.sepY   = (int16_t)midTop;
    f.sepH   = (int16_t)(in.chart ? midBot - 8 - midTop : 0);
    f.groupY = (int16_t)midTop;
    f.heroY  = (int16_t)(midTop + 18);
    f.battX  = (int16_t)(f.colLX + f.colLW - 48);
    f.battY  = (int16_t)(midTop - 2);
    f.inX    = f.colLX;
    f.inW    = f.colLW;
    f.inRuleY = (int16_t)(in.nIn ? midBot - KDF_IN_H : midBot);

    // The largest headline under which the grid still sets at the upright
    // page's sizes (34, or 27 three across); if none does, the one that sets
    // it largest.
    const int outBot = f.inRuleY - 8;
    const int n = in.nGrid > 6 ? 6 : in.nGrid;
    int pick = -1, bestG = KDF_LAND_GROW_MIN, bestV = -1;
    for (int g = KDF_GROW_MAX; g >= KDF_LAND_GROW_MIN; g -= 20) {
        kdFlowType(g, f);
        kdFlowOutdoor(in, outBot, f);
        const bool ok = n ? f.gridValSz >= (f.gridRows[0] >= 3 ? 27 : 34) * kdfPct(in.outPct) / 100
                          : f.subY + (in.sub ? f.subSz : 0) <= outBot;
        if (ok) { pick = g; break; }
        if (n && f.gridValSz >= bestV) { bestV = f.gridValSz; bestG = g; }
    }
    if (pick < 0) pick = bestG;
    kdFlowType(pick, f);
    kdFlowOutdoor(in, outBot, f);
    kdFlowIndoor(in, midBot - 8, f);

    // ── The chart, beside the readings ──
    f.rule2X = (int16_t)(KDF_LAND_SEP + 10);
    f.rule2W = 0;                         // the hairline is the separator
    f.labChartX = f.rule2X;
    f.grX    = (int16_t)(KDF_LAND_SEP + 12);
    f.grW    = (int16_t)(X1 - 2 - f.grX);
    f.keyInX = (int16_t)(f.grX + f.grW - 100);
    f.keyBand = false;
    if (in.chart) {
        f.rule2Y = (int16_t)(midTop - 6);
        f.grY    = (int16_t)(midTop + 20);
        f.grH    = (int16_t)(midBot - KDF_CHART_BELOW - 4 - f.grY);
    } else {
        f.rule2Y = f.rule3Y;
        f.grY = f.rule3Y;
        f.grH = 0;
    }

    // ── The forecast band: five outlook columns ──
    f.olN = 5;
    for (int i = 0; i < KDF_OL_MAX; i++) f.olX[i] = (int16_t)(X0 + 308 + KDF_OL_PITCH * i);
}

/// The upright page's x, which are the layout file's.
static inline void kdFlowUpright(const KdFlowIn& in, KdFlow& f) {
    static const int16_t ol[3] = {320, 410, 500};
    f.land   = false;
    f.pageW  = 600; f.pageH = (int16_t)KDF_PAGE_H;
    f.groupY = (int16_t)KDF_TOP_Y;
    f.colLX  = 18;
    f.colLW  = (int16_t)(!in.clock && !in.nIn ? 564 : KDF_COL_L);
    f.inX = 318;  f.inW = 264;
    f.sepX = 300; f.sepY = (int16_t)KDF_TOP_Y;
    f.clX = 318;  f.clY = (int16_t)KDF_CL_Y; f.clW = 264;
    f.topRowY = 0;
    f.rule2X = 18; f.rule2W = 564;
    f.labChartX = 18;
    f.grX = 20;   f.grW = 560;
    f.keyInX = 470;
    f.keyBand = true;
    f.olN = 3;
    for (int i = 0; i < KDF_OL_MAX; i++) f.olX[i] = i < 3 ? ol[i] : 0;
    f.wkX = 18;   f.wkCellW = 81;
    f.wkRule = true;
    f.footY = (int16_t)KDF_FOOT_Y;
    f.statX = 396;
    f.battX = (int16_t)(f.colLX + f.colLW - 48);
    f.battY = 18;
    f.headW = f.colLW;
}

// ---------------------------------------------------------------------------
// The wall page, 600 x 800
// ---------------------------------------------------------------------------
// The desk page read from across a room. Its headline shares a 270 px column
// with the clock's, so it cannot pass about 77 px whatever is switched off:
// the height a section gives back goes to the chart, or to air. This page
// spends the width instead:
//
//   OUTDOOR  21.7° / 37%              the headline across the whole page
//            14.2 – 22.8° · 2 min
//   ─────────────────────────────────
//   the grid          ┃ INDOOR        two columns
//   1013 hPa          ┃ 23.5°         the indoor readings one under the other
//   8.4°              ┃ 45%
//   ─────────────────────────────────
//   17:40             ┃ FORECAST      the clock beside the current conditions
//   ─────────────────────────────────
//   the footer
//
// No chart and no week strip: a line one pixel wide does not read at three
// metres, and the room it took is the room the readings want. Every size is
// the most its place allows, as everywhere else here.
static const int KDF_WALL_X1      = 582;   ///< the right margin
static const int KDF_WALL_W       = 564;   ///< margin to margin
static const int KDF_WALL_COL     = 352;   ///< the left column: the grid, the clock
static const int KDF_WALL_SEP     = 376;   ///< the hairline between the columns
static const int KDF_WALL_RX      = 394;   ///< the right column
static const int KDF_WALL_LAB     = 18;    ///< the captions and the headings
static const int KDF_WALL_HERO    = 160;
static const int KDF_WALL_BIG     = 96;
static const int KDF_WALL_SUB     = 22;
static const int KDF_WALL_BAND    = 200;   ///< the clock and the forecast, rule to footer
static const int KDF_WALL_CLOCK   = 1300;  ///< the clock beside the forecast: 96 -> 124
static const int KDF_WALL_CLOCK_1 = 1500;  ///< ...and alone in the band: 96 -> 144
static const int KDF_WALL_FC_TEXT = 28;    ///< the summary, at most
static const int KDF_WALL_FC_TEMP = 60;    ///< the day's high and low, at most
static const int KDF_IN_V_GAP     = 10;    ///< one indoor reading's foot to the next one's caption

/// The day's high and low as the band prints them, "-00°/-00°", in mille.
static const int KDF_FC_TEMP_ADV  = 4 * 620 + 2 * 330 + 330 + 2 * 330;

/// The indoor readings one under the other, between `top` and `bot`, f.inW
/// across: the heading, the first reading with no caption under it, and each
/// of the others with its caption, at seven tenths of the first. As large as
/// the column's width and height allow, never larger than the headline, and
/// the whole stack centred in the room.
static inline void kdFlowIndoorV(const KdFlowIn& in, int top, int bot, KdFlow& f) {
    f.inVcol = true;
    f.inCol = false;
    f.inStack = false;
    f.inW1Pm = 1000;
    f.inValSz1 = f.inValSz = 0;
    f.inLabY = (int16_t)top;
    f.inValY = f.inVal2Y = f.inVal3Y = (int16_t)(top + f.labSz + 8);
    const int m = in.nIn > 3 ? 3 : in.nIn;
    if (m == 0) return;
    const int W  = f.inW - KDF_CELL_PAD;
    const int a1 = in.inAdv[0] ? in.inAdv[0] : 1000;
    int aR = 0;
    for (int i = 1; i < m; i++) aR = kdfMax(aR, in.inAdv[i] ? in.inAdv[i] : 1000);
    const int head = f.labSz + 8;                 // the heading, to the first value
    const int per  = KDF_IN_V_GAP + f.labSz + 4;  // each further caption
    const int room = bot - top;

    int s1 = kdfMin(f.heroSz, W * 1000 / a1);
    if (aR) s1 = kdfMin(s1, (W * 1000 / aR) * 10 / 7);
    while (s1 > 20 && head + s1 + (m - 1) * (per + s1 * 7 / 10) > room) s1--;
    s1 = kdfMax(20, s1 * kdfPct(in.inPct) / 100);
    const int s = s1 * 7 / 10;

    const int need = head + s1 + (m - 1) * (per + s);
    f.inLabY   = (int16_t)(top + kdfMax(0, (room - need) / 2));
    f.inValY   = (int16_t)(f.inLabY + head);
    f.inVal2Y  = (int16_t)(f.inValY + s1 + per);
    f.inVal3Y  = (int16_t)(f.inVal2Y + s + per);
    f.inValSz1 = (uint8_t)s1;
    f.inValSz  = (uint8_t)s;
}

static inline void kdFlowWall(const KdFlowIn& in, KdFlow& f) {
    const int X0 = 18, X1 = KDF_WALL_X1;
    f.wall  = true;
    f.chart = false;
    f.week  = false;
    f.olN   = 0;
    for (int i = 0; i < KDF_OL_MAX; i++) f.olX[i] = 0;
    f.labSz = (uint8_t)KDF_WALL_LAB;

    // ── The headline, across the page ──
    f.groupY  = (int16_t)KDF_TOP_Y;
    f.heroY   = (int16_t)(KDF_TOP_Y + f.labSz + 8);
    f.colLX   = (int16_t)X0;
    f.colLW   = (int16_t)KDF_WALL_W;
    f.headW   = (int16_t)KDF_WALL_W;
    f.heroSz  = (uint8_t)KDF_WALL_HERO;
    f.bigSz   = (uint8_t)KDF_WALL_BIG;
    f.headGap = 12;
    f.slashW  = 30;
    f.subSz   = (uint8_t)KDF_WALL_SUB;
    kdFlowHeadFit(in, f);
    f.grow    = (uint16_t)(f.heroSz * 1000 / 88);
    // THE ROW IS AS TALL AS THE LARGEST HEADLINE, whatever this one came out
    // at, and the headline stands on its foot. Sized by what it prints, it is
    // smaller at "-12.5°" than at "8.4°"; a row that followed it would move
    // everything under it, and repaint the whole panel, each time the
    // reading gained a figure.
    f.subY    = (int16_t)(f.heroY + KDF_WALL_HERO + 4);
    f.heroY   = (int16_t)(f.heroY + KDF_WALL_HERO - f.heroSz);
    f.headRuleY = (int16_t)(in.sub ? f.subY + f.subSz + 12 : f.subY + 4);
    f.battX   = (int16_t)(X0 + KDF_WALL_W - 48);
    f.battY   = 18;

    // ── The band at the foot: the clock and the forecast ──
    const bool band = in.clock || in.forecast;
    f.rule3Y = (int16_t)(band ? KDF_FOOT_Y - KDF_WALL_BAND : KDF_FOOT_Y);
    f.rule2Y = f.rule3Y;
    f.rule2W = 0;
    f.grY = f.rule3Y;
    f.grH = 0;
    f.wkRuleY = (int16_t)KDF_FOOT_Y;

    // ── Between them: the grid on the left, the indoor readings on the right ──
    const int midTop = f.headRuleY + 14;
    const int midBot = f.rule3Y - 12;
    f.topBot = f.rule3Y;
    const bool both = in.nGrid > 0 && in.nIn > 0;
    f.colLW = (int16_t)(both ? KDF_WALL_COL : KDF_WALL_W);
    // ONE ARRANGEMENT PER COUNT, as large as its cells allow: two one under
    // the other, then two to a row — four 2 x 2, six three rows of two, an
    // odd one alone on the last row.
    const int nG = in.nGrid > 6 ? 6 : in.nGrid;
    kdFlowGrid(in, midTop, midBot, f, nG <= 2 ? nG : (nG + 1) / 2);
    f.inX = (int16_t)(in.nGrid > 0 ? KDF_WALL_RX : X0);
    f.inW = (int16_t)(in.nGrid > 0 ? X1 - KDF_WALL_RX : KDF_WALL_W);
    f.inRuleY = (int16_t)midTop;
    kdFlowIndoorV(in, midTop, midBot, f);
    f.sepX = (int16_t)KDF_WALL_SEP;
    f.sepY = (int16_t)midTop;
    f.sepH = (int16_t)(both ? midBot - midTop : 0);

    // ── The clock ──
    const bool beside = in.clock && in.forecast;
    kdFlowClockSizes(beside ? KDF_WALL_CLOCK : KDF_WALL_CLOCK_1, f);
    // Alone, in the middle of the band — by "00:00", as the clock's own
    // centring goes by a sample of the time — and its box as wide as that,
    // in the middle too: the box is what the boxed style fills and what the
    // panel refreshes, so starting it there and keeping the band's width ran
    // it 85 px past the right edge of the screen.
    const int clIn = beside ? 0 : kdfMax(0, (KDF_WALL_W - f.clSize * 2740 / 1000) / 2);
    f.clX = (int16_t)(X0 + clIn);
    f.clW = (int16_t)((beside ? KDF_WALL_COL : KDF_WALL_W) - 2 * clIn);
    f.clY = (int16_t)(f.rule3Y + kdfMax(0, (KDF_WALL_BAND - f.clH) / 2));
    f.sep2X = (int16_t)KDF_WALL_SEP;
    f.sep2Y = (int16_t)(f.rule3Y + 12);
    f.sep2H = (int16_t)(beside ? KDF_WALL_BAND - 24 : 0);

    // ── The forecast: the icon and the summary, the day under them ──
    const int fx = in.clock ? KDF_WALL_RX : X0;
    const int fw = X1 - fx;
    f.labFcX  = (int16_t)fx;
    f.labFcY  = (int16_t)(f.rule3Y + 12);
    f.fcIconX = (int16_t)fx;
    f.fcIconY = (int16_t)(f.rule3Y + 42);
    f.fcTextX = (int16_t)(fx + 64);
    f.fcTextW = (int16_t)(fw - 64);
    f.fcTextSz = (uint8_t)KDF_WALL_FC_TEXT;
    f.fcTextY = (int16_t)(f.fcIconY + (52 - KDF_WALL_FC_TEXT) / 2);
    f.fcTempX = (int16_t)fx;
    f.fcTempY = (int16_t)(f.rule3Y + 104);
    f.fcTempSz = (uint8_t)kdfMin(KDF_WALL_FC_TEMP, fw * 1000 / KDF_FC_TEMP_ADV);
    f.fcWindX = (int16_t)fx;
    f.fcWindY = (int16_t)(f.fcTempY + f.fcTempSz + 8);
    f.fcWindSz = 17;
}

/// The forecast band's current conditions where the layout file has them —
/// the desk page, upright or turned.
static inline void kdFlowFcDesk(KdFlow& f) {
    f.labFcX  = 18;  f.labFcY  = (int16_t)(f.rule3Y + 6);
    f.fcIconX = 18;  f.fcIconY = (int16_t)(f.rule3Y + 28);
    f.fcTextX = 78;  f.fcTextY = (int16_t)(f.rule3Y + 28);
    f.fcTextW = 0;   f.fcTextSz = 31;
    f.fcTempX = 78;  f.fcTempY = (int16_t)(f.rule3Y + 62);  f.fcTempSz = 33;
    f.fcWindX = 78;  f.fcWindY = (int16_t)(f.rule3Y + 100); f.fcWindSz = 17;
}

/// The whole page. `footY` is where the landscape page's footer goes — the
/// browser's is higher; the upright page ignores it.
static inline KdFlow kdFlowComputeAt(const KdFlowIn& in, int footY) {
    KdFlow f;
    memset(&f, 0, sizeof(f));
    f.chart    = in.chart;
    f.forecast = in.forecast;
    f.week     = in.week;
    f.clock    = in.clock;
    if (in.land) {
        kdFlowLand(in, footY, f);
        f.headW = f.colLW;
        kdFlowFcDesk(f);
        return f;
    }
    kdFlowUpright(in, f);
    if (in.wall) {
        kdFlowWall(in, f);
        return f;
    }

    // Stacked from the bottom: the footer, the week strip, the forecast.
    f.wkRuleY = (int16_t)(in.week ? KDF_FOOT_Y - KDF_WEEK_H : KDF_FOOT_Y);
    f.wkHdgY  = (int16_t)(f.wkRuleY + 5);
    f.wkY     = (int16_t)(f.wkRuleY + 24);
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
        KdFlow at = f;
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
    kdFlowFcDesk(f);
    return f;
}

static inline KdFlow kdFlowCompute(const KdFlowIn& in) {
    return kdFlowComputeAt(in, KDF_LAND_FOOT_Y);
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
///
/// `resW` is the panel's SHORT side — its width held upright — on either page:
/// the landscape page is 800 x 600 design pixels on a 1448 x 1072 panel, and
/// the scale is the same 1072 / 600 it is upright.
///
/// The upright page sends only heights, as it always has: its x are the layout
/// file's, which were measured on each panel and are not all a plain scale of
/// the 600 px file's. The landscape page sends its x too.
static const int KDF_PANEL_BASE = 50;
static const int KDF_PANEL_KEYS = KDF_PANEL_BASE + 48;
/// One key into kdFlowPanelKeys()'s list. Out of line on purpose: the list
/// is over a hundred keys, and the bounds check and the scaling written out at
/// each of them cost the all-features C3 build kilobytes it does not have.
struct KdfPut { KdFlowKV* out; int n; unsigned resW; };
static inline __attribute__((noinline)) void kdfPut(KdfPut& w, const char* k, int v) {
    // Bounded: a key added below without KDF_PANEL_KEYS growing with it is
    // dropped (and fails test_panel_keys) rather than written past `out`.
    if (w.n < KDF_PANEL_KEYS) { w.out[w.n].key = k; w.out[w.n].value = v; }
    w.n++;
}
static inline __attribute__((noinline)) void kdfPutK(KdfPut& w, const char* k, int v) {
    kdfPut(w, k, kdFlowPanel(v, w.resW));
}

/// The keys that are a field as it stands, as a table rather than a call each:
/// the field's place in KdFlow and its width, and whether it is a size the
/// panel scales (KDF_K) or a count or switch it takes as it is (KDF_R).
struct KdfKey { const char* key; uint16_t off; uint8_t size; uint8_t scaled; };
#define KDF_T(k, fld, sc) \
    { (k), (uint16_t)offsetof(KdFlow, fld), (uint8_t)sizeof(((KdFlow*)0)->fld), (sc) }
static inline __attribute__((noinline)) void kdfPutTable(KdfPut& w, const KdFlow& f,
                                                         const KdfKey* t, int n) {
    const uint8_t* b = (const uint8_t*)&f;
    for (int i = 0; i < n; i++) {
        int v;
        if (t[i].size == 1) {
            v = b[t[i].off];
        } else {
            int16_t h;
            memcpy(&h, b + t[i].off, sizeof(h));
            v = h;
        }
        if (t[i].scaled) kdfPutK(w, t[i].key, v);
        else             kdfPut(w, t[i].key, v);
    }
}

static inline int kdFlowPanelKeys(const KdFlow& f, unsigned resW, KdFlowKV* out) {
    KdfPut w = {out, 0, resW};
    #define KDF_K(k, v)  kdfPutK(w, (k), (v))
    #define KDF_R(k, v)  kdfPut(w, (k), (v))
    static const KdfKey kBase[] = {
        KDF_T("GROUP_LAB_SZ",  labSz, 1),
        KDF_T("HERO_Y",        heroY, 1),
        KDF_T("HERO_SZ",       heroSz, 1),
        KDF_T("BIG_SZ",        bigSz, 1),
        KDF_T("HEAD_GAP",      headGap, 1),
        KDF_T("SLASH_W",       slashW, 1),
        KDF_T("SUB_Y",         subY, 1),
        KDF_T("SUB_SZ",        subSz, 1),
        KDF_T("GRID_Y",        gridY, 1),
        KDF_T("GRID_ROW_H",    gridRowH, 1),
        KDF_T("GRID_LAB_SZ",   labSz, 1),
        KDF_T("GRID_VAL_SZ",   gridValSz, 1),
        KDF_T("GRID_VAL_SZ_3", gridValSz, 1),
        KDF_T("SEP_H",         sepH, 1),
        KDF_T("CL_SIZE",       clSize, 1),
        KDF_T("CL_H",          clH, 1),
        KDF_T("CL_SZ_BOXED",   clBoxed, 1),
        KDF_T("CL_SZ_RULED",   clRuled, 1),
        KDF_T("CL_RULED_PAD",  clRuledPad, 1),
        KDF_T("CL_SZ_DATED",   clDated, 1),
        KDF_T("CL_DATE_SZ",    clDateSz, 1),
        KDF_T("CL_DATE_GAP",   clDateGap, 1),
        KDF_T("IN_RULE_Y",     inRuleY, 1),
        KDF_T("IN_LAB_Y",      inLabY, 1),
        KDF_T("IN_VAL_Y",      inValY, 1),
        KDF_T("IN_VAL2_Y",     inVal2Y, 1),
        KDF_T("IN_VAL_SZ",     inValSz, 1),
        KDF_T("IN_VAL_SZ_1",   inValSz1, 1),
        KDF_T("IN_W1",         inW1Pm, 0),
        KDF_T("IN_VAL3_Y",     inVal3Y, 1),
        KDF_T("RULE2_Y",       rule2Y, 1),
        KDF_T("GR_Y",          grY, 1),
        KDF_T("GR_H",          grH, 1),
        KDF_T("RULE3_Y",       rule3Y, 1),
        KDF_T("LAB_FC_Y",      labFcY, 1),
        KDF_T("FC_ICON_Y",     fcIconY, 1),
        KDF_T("FC_TEXT_Y",     fcTextY, 1),
        KDF_T("FC_TEMP_Y",     fcTempY, 1),
        KDF_T("FC_WIND_Y",     fcWindY, 1),
        KDF_T("WK_HDG_Y",      wkHdgY, 1),
        KDF_T("WK_Y",          wkY, 1),
    };
    kdfPutTable(w, f, kBase, sizeof(kBase) / sizeof(kBase[0]));
    KDF_R("IN_STACK",     f.inStack ? 1 : 0);
    KDF_R("IN_COL",       f.inCol ? 1 : 0);
    KDF_K("LAB_CHART_Y",  f.rule2Y + 6);
    KDF_K("KEY_Y",        f.grY + f.grH + 2);
    KDF_K("OL0_Y",        f.rule3Y + 12);
    KDF_K("OL1_Y",        f.rule3Y + 12);
    KDF_K("OL2_Y",        f.rule3Y + 12);
    KDF_K("WK_HDG_RULE_Y", f.wkHdgY - 5);
    KDF_R("FC_BAND",      f.forecast ? 1 : 0);

    if (f.land) {
        const int X1 = KDF_LAND_X1;
        KDF_R("LAND",         1);
        static const KdfKey kLand[] = {
            KDF_T("TOP_Y",         groupY, 1),
            KDF_T("COL_L_X",       colLX, 1),
            KDF_T("COL_L_W",       colLW, 1),
            KDF_T("COL_R_X",       inX, 1),
            KDF_T("COL_R_W",       inW, 1),
            KDF_T("SEP_X",         sepX, 1),
            KDF_T("CL_X",          clX, 1),
            KDF_T("CL_Y",          clY, 1),
            KDF_T("CL_W",          clW, 1),
            KDF_T("TOPROW_Y",      topRowY, 1),
            KDF_T("RULE2_X",       rule2X, 1),
            KDF_T("LAB_CHART_X",   labChartX, 1),
            KDF_T("GR_X",          grX, 1),
            KDF_T("KEY_IN_X",      keyInX, 1),
            KDF_T("OL_N",          olN, 0),
            KDF_T("OL0_X",         olX[0], 1),
            KDF_T("OL1_X",         olX[1], 1),
            KDF_T("OL2_X",         olX[2], 1),
            KDF_T("OL3_X",         olX[3], 1),
            KDF_T("OL4_X",         olX[4], 1),
            KDF_T("WK_X",          wkX, 1),
            KDF_T("WK_HDG_X",      wkX, 1),
            KDF_T("WK_CELL_W",     wkCellW, 1),
            KDF_T("FOOT_RULE_Y",   footY, 1),
            KDF_T("STAT_X",        statX, 1),
            KDF_T("BATT_X",        battX, 1),
            KDF_T("BATT_Y",        battY, 1),
        };
        kdfPutTable(w, f, kLand, sizeof(kLand) / sizeof(kLand[0]));
        KDF_R("RULE2_W",      0);
        // The image's own width, which /kindle/graph.bmp rounds to 8.
        KDF_R("GR_W",         kdFlowPanel(f.grW, resW) & ~7);
        KDF_R("KEY_BAND",     f.keyBand ? 1 : 0);
        KDF_K("OL3_Y",        f.rule3Y + 12);
        KDF_K("OL4_Y",        f.rule3Y + 12);
        KDF_K("RULE3_W",      X1 - 18);
        KDF_R("WK_HDG_RULE_W", 0);
        KDF_K("FOOT_RULE_W",  X1 - 18);
        KDF_K("FOOT_Y",       f.footY + 8);
        KDF_K("STAT_Y",       f.footY + 8);
    } else if (f.wall) {
        // Everything that moves, as on its side — and the inks and weights,
        // which the script derives from WALL=1.
        KDF_R("WALL",         1);
        static const KdfKey kWall[] = {
            KDF_T("TOP_Y",         groupY, 1),
            KDF_T("COL_L_X",       colLX, 1),
            KDF_T("COL_L_W",       colLW, 1),
            KDF_T("HEAD_W",        headW, 1),
            KDF_T("HEAD_RULE_Y",   headRuleY, 1),
            KDF_T("COL_R_X",       inX, 1),
            KDF_T("COL_R_W",       inW, 1),
            KDF_T("SEP_X",         sepX, 1),
            KDF_T("SEP_Y",         sepY, 1),
            KDF_T("SEP2_X",        sep2X, 1),
            KDF_T("SEP2_Y",        sep2Y, 1),
            KDF_T("SEP2_H",        sep2H, 1),
            KDF_T("CL_X",          clX, 1),
            KDF_T("CL_Y",          clY, 1),
            KDF_T("CL_W",          clW, 1),
            KDF_T("LAB_FC_X",      labFcX, 1),
            KDF_T("FC_ICON_X",     fcIconX, 1),
            KDF_T("FC_TEXT_X",     fcTextX, 1),
            KDF_T("FC_TEXT_W",     fcTextW, 1),
            KDF_T("FC_TEXT_SZ",    fcTextSz, 1),
            KDF_T("FC_TEMP_X",     fcTempX, 1),
            KDF_T("FC_TEMP_SZ",    fcTempSz, 1),
            KDF_T("FC_WIND_X",     fcWindX, 1),
            KDF_T("FC_WIND_SZ",    fcWindSz, 1),
            KDF_T("BATT_X",        battX, 1),
            KDF_T("BATT_Y",        battY, 1),
        };
        kdfPutTable(w, f, kWall, sizeof(kWall) / sizeof(kWall[0]));
        KDF_R("IN_VCOL",      f.inVcol ? 1 : 0);
        KDF_R("OL_N",         0);
    } else if (f.colLW != KDF_COL_L) {
        // Upright with nothing in the right column: the outdoor one is wider.
        static const KdfKey kNarrow[] = {
            KDF_T("COL_L_W",       colLW, 1),
            KDF_T("BATT_X",        battX, 1),
        };
        kdfPutTable(w, f, kNarrow, sizeof(kNarrow) / sizeof(kNarrow[0]));
    }
    #undef KDF_K
    #undef KDF_R
    #undef KDF_T
    return w.n > KDF_PANEL_KEYS ? -1 : w.n;  // -1: the table outgrew its count
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
    // Landscape: the chart and the week strip are beside something as tall as
    // they are, so only the bands across the page cost the browser more.
    if (f.land)
        return KDF_HTML_FOOT + KDF_HTML_SPARE + (f.forecast ? KDF_HTML_FC : 0);
    return KDF_HTML_FOOT + KDF_HTML_SPARE + (f.chart ? KDF_HTML_CHART : 0) +
           (f.forecast ? KDF_HTML_FC : 0) + (f.week ? KDF_HTML_WEEK : 0);
}

/// The layout the browser page draws. With a chart it is the panel's, less
/// kdFlowHtmlExtra() off the chart; without one the top block is laid out
/// again that much shorter, so what is in it is sized for the room it gets.
///
/// The landscape page is laid out again with its footer that much higher: its
/// readings and its chart sit side by side, so both give the height back.
static inline KdFlow kdFlowComputeHtml(const KdFlowIn& in) {
    KdFlow f = kdFlowCompute(in);
    if (f.land) return kdFlowComputeAt(in, KDF_LAND_FOOT_Y - kdFlowHtmlExtra(f));
    if (!f.chart) kdFlowTop(in, f.topBot - KDF_TOP_Y - kdFlowHtmlExtra(f), f);
    return f;
}

/// The height of the top block's two cells on the browser page — on the
/// landscape page, of the readings' cell beside the chart.
static inline int kdFlowHtmlColH(const KdFlow& f) {
    if (f.land) return f.topBot - f.groupY - 12;
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
        // In two columns the second cell is the taller, and the row's top is
        // its upper caption, not the first field's top.
        const int rowTop = f.inCol ? f.inVal2Y - f.labSz - 4 : f.inValY;
        out += ".inrow{margin-top:";
        KDF_PX(kdfMax(0, 8 + rowTop - (f.inLabY + f.labSz + 18))); out += "}";
        out += ".inrow2{margin-top:"; KDF_PX(10); out += "}";
        out += ".inrow td.c1{text-align:center}";
        if (f.inCol) {
            // The gap from the upper value's foot to the lower one's caption.
            out += ".inb{margin-top:";
            KDF_PX(f.inVal3Y - f.labSz - 4 - (f.inVal2Y + f.inValSz)); out += "}";
        }
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
    out += ".clock-x{font-size:";   KDF_PX(kdfScale(44, kdfMin(f.grow, KDF_GROW_BIG)));
    out += ";line-height:";         KDF_PX(100 * gc / 1000); out += "}";
    // The landscape page's rows and columns — see kdFlowLand().
    if (f.land) {
        if (f.topRowY > KDF_TOP_Y) {
            out += ".trow{height:"; KDF_PX(f.topRowY - KDF_TOP_Y); out += "}";
        }
        out += ".trow td{vertical-align:middle;padding:0}.trow .wk{margin-top:0}";
        if (f.clock && f.week) { out += ".tclk{width:"; KDF_PX(f.wkX - f.colLX); out += "}"; }
        else                     out += ".tclk{text-align:center}";
        if (f.chart) {
            out += ".col-l{width:";      KDF_PX(f.sepX - f.colLX);
            out += "}.top .sep{padding-left:"; KDF_PX(f.grX - f.sepX - 2); out += "}";
        }
    }
    #undef KDF_PX
}

/// The chart's height on the browser page, in design pixels.
static inline int kdFlowHtmlChartH(const KdFlow& f) {
    if (f.land) return f.chart ? f.grH : 0;     // kdFlowComputeHtml() took it off already
    return f.chart ? f.grH - kdFlowHtmlExtra(f) : 0;
}
