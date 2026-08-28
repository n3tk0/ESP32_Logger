// ============================================================================
// src/web/KindleSkin.h
//
// The runtime half of the Kindle dashboard's appearance: the face, which
// figures are set bold, how the clock is drawn, and how the time, the date and
// the pressure are written. The values live in config.kindle (see Config.h)
// and reach the page through here.
//
// WHY THIS IS AN OVERRIDE BLOCK AND NOT BRANCHES IN THE STYLESHEET
// ----------------------------------------------------------------
// KindleDashboard.cpp emits the base stylesheet as a run of KD_S / KD_N calls,
// and that sheet IS the design: every number in it was measured at a 600 px
// layout. Two things depend on it staying a flat, unconditional run:
//
//   1. tools/kindle_preview/preview.py reconstructs the sheet by reading those
//      calls out of the source. Branches would put every arm of every `if`
//      into the extracted CSS at once, and the preview would silently stop
//      being a picture of the page.
//   2. Reading the sheet against the design means reading it top to bottom.
//      Conditionals through it would make "what does .big actually say" a
//      question about runtime state.
//
// So the sheet stays as it was, and everything here is emitted AFTER it as
// overrides. A device on defaults emits an empty block and renders exactly
// what the design says. That is also why the defaults are the design rather
// than something more opinionated: nobody's page changes because the firmware
// learned it could be changed.
//
// WHAT IS DELIBERATELY NOT A SETTING
// ----------------------------------
// Sizes, spacing, the greys, and the page width. The first three are what make
// the page readable from across a room and were arrived at by measuring in a
// browser, not by taste; exposing them offers mainly a way to break the page.
// The width stays KINDLE_PAGE_W because every size in the sheet is derived
// from it at compile time — see GET /kindle/probe for choosing it.
// ============================================================================
#pragma once

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../core/Config.h"
#include "KindleDashboard.h"     // kdPx()

#ifdef FEATURE_KINDLE_DASHBOARD
#include "DashboardStrings.h"    // kdMonth()
#endif

// ---------------------------------------------------------------------------
// The face
// ---------------------------------------------------------------------------
// Every stack ends in a generic family, because none of these names is
// guaranteed: which faces the browser can reach depends on the reader's
// firmware, and a stack that resolves to nothing renders in whatever the
// browser defaults to rather than in the face that was chosen.
//
// Bookerly first in the default stack is not decoration — it is the face the
// device reads books in, so it is the one a reader's eye is already trained on
// and the one most likely to be present.
inline const char* kdFaceStack(const KindleConfig& k) {
    switch (k.face) {
        case KFACE_CAECILIA:    return "Caecilia,'PMN Caecilia',Bookerly,Georgia,serif";
        case KFACE_PALATINO:    return "Palatino,'Palatino Linotype',Georgia,serif";
        case KFACE_BASKERVILLE: return "Baskerville,'Libre Baskerville',Georgia,serif";
        case KFACE_HELVETICA:   return "Helvetica,'Helvetica Neue',Arial,sans-serif";
        case KFACE_FUTURA:      return "Futura,'Century Gothic',Avenir,sans-serif";
        case KFACE_CUSTOM:      return nullptr;   // caller uses faceCustom
        case KFACE_BOOKERLY:
        default:                return "Bookerly,Caecilia,Georgia,'Times New Roman',serif";
    }
}

/// True when `k` asks for nothing the base stylesheet does not already do.
inline bool kdSkinIsDefault(const KindleConfig& k) {
    return k.face == KFACE_BOOKERLY && k.boldZones == 0 && k.clockStyle == KCLOCK_PLAIN;
}

// ---------------------------------------------------------------------------
// The override block
// ---------------------------------------------------------------------------
// Appended to the stylesheet after everything the design emits. Nothing is
// written for a setting that is already the default, so the common page ships
// no extra bytes at all.
template <typename StringT>
inline void kdSkinCss(StringT& out, const KindleConfig& k) {
    // ── Face ────────────────────────────────────────────────────────────────
    // .ax as well as body: the chart's axis labels are SVG text, and SVG text
    // does not inherit font-family from an HTML ancestor in every engine this
    // page may meet. Left out, the chart would keep the old face while the
    // page around it changed.
    if (k.face != KFACE_BOOKERLY) {
        const char* stack = kdFaceStack(k);
        if (!stack) {
            // A custom stack is written by a person into a text field and
            // lands verbatim in a stylesheet, so it is fenced at both ends:
            // sanitised on the way in (see the API handler) and ignored here
            // if it is empty.
            stack = k.faceCustom[0] ? k.faceCustom : nullptr;
        }
        if (stack) {
            out += "body,.ax{font-family:";
            out += stack;
            out += "}";
        }
    }

    // ── Weight ──────────────────────────────────────────────────────────────
    // One rule per zone rather than one long selector list, because a browser
    // that chokes on an unknown selector drops the rule it is in and no more.
    #define KD_BOLD(bit, sel) if (k.boldZones & (bit)) { out += sel; out += "{font-weight:bold}"; }
    KD_BOLD(KBOLD_OUT_TEMP, ".big")
    KD_BOLD(KBOLD_OUT_HUM,  ".hum-o")
    KD_BOLD(KBOLD_PRESSURE, ".pres")
    KD_BOLD(KBOLD_CLOCK,    ".clock")
    KD_BOLD(KBOLD_IN_TEMP,  ".in-t")
    KD_BOLD(KBOLD_IN_HUM,   ".hum-i")
    KD_BOLD(KBOLD_FORECAST, ".fc-t,.per-t")
    KD_BOLD(KBOLD_WEEK,     ".wd-d")
    KD_BOLD(KBOLD_LABELS,   ".lab,.sec,.per-l,.wd-n")
    #undef KD_BOLD

    // ── The clock ───────────────────────────────────────────────────────────
    // Each of these keeps the block's total height at the 139 px the design
    // fixed, because that height is what puts the hairline under it level with
    // the outdoor column's text. Anything taller makes the page ragged; taller
    // still and the footer goes below the fold. box-sizing is border-box for
    // the whole page, so a border added here comes out of the padding rather
    // than adding to the height.
    switch (k.clockStyle) {
        case KCLOCK_BOXED:
            // The same treatment the current day already gets in the week
            // strip: inverted, because a filled block is the one mark that
            // survives e-ink dithering unambiguously.
            out += ".clock{background:#000;color:#fff;text-align:center;height:";
            out += kdPx(131);
            out += "px;margin-bottom:";
            out += kdPx(8);
            out += "px;letter-spacing:";
            out += kdPx(-2);
            out += "px}";
            break;

        case KCLOCK_RULED:
            // A hairline above; the .inrule already under the block is the one
            // below. Smaller and wider-tracked, which is what makes it read as
            // a rule rather than as a number that happens to have a line over
            // it.
            out += ".clock{border-top:";
            out += kdPx(1);
            out += "px solid #000;text-align:center;font-size:";
            out += kdPx(78);
            out += "px;letter-spacing:";
            out += kdPx(2);
            out += "px;padding-top:";
            out += kdPx(18);
            out += "px}";
            break;

        case KCLOCK_DATED:
            // Room for a date line is taken FROM the clock, not added under
            // it: 112 + 27 is the same 139.
            out += ".clock{height:";
            out += kdPx(112);
            out += "px;font-size:";
            out += kdPx(84);
            out += "px;line-height:";
            out += kdPx(92);
            out += "px}.clock-d{height:";
            out += kdPx(27);
            out += "px;font-size:";
            out += kdPx(16);
            out += "px;letter-spacing:";
            out += kdPx(1);
            out += "px;color:#444}";
            break;

        case KCLOCK_PLAIN:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// The time
// ---------------------------------------------------------------------------
// No seconds in any of these. The page repaints on a timer measured in
// minutes; a seconds field would be wrong the instant it was drawn and would
// invite the reader to watch a number that never moves.
inline void kdFmtTime(char* buf, size_t n, const struct tm& t, uint8_t fmt) {
    switch (fmt) {
        case KTIME_24_LEAN:
            snprintf(buf, n, "%d:%02d", t.tm_hour, t.tm_min);
            break;
        case KTIME_12: {
            int h = t.tm_hour % 12;
            if (h == 0) h = 12;
            // Lower case and thin-spaced: "AM" set in the same size as the
            // numerals would compete with them, and this page has one thing
            // to say per line.
            snprintf(buf, n, "%d:%02d%s", h, t.tm_min, t.tm_hour < 12 ? "am" : "pm");
            break;
        }
        case KTIME_24:
        default:
            snprintf(buf, n, "%02d:%02d", t.tm_hour, t.tm_min);
            break;
    }
}

// ---------------------------------------------------------------------------
// The date
// ---------------------------------------------------------------------------
// Only KCLOCK_DATED draws this today. Written as its own function anyway
// because the week strip's numerals carry the date on every other style, and
// the day this page grows a second date line it should be written the same
// way as the first.
inline void kdFmtDate(char* buf, size_t n, const struct tm& t, uint8_t fmt) {
#ifdef FEATURE_KINDLE_DASHBOARD
    const char* mon = kdMonth(t.tm_mon);
#else
    const char* mon = "";
#endif
    switch (fmt) {
        case KDATE_MONTH_DAY:
            snprintf(buf, n, "%s %d", mon, t.tm_mday);
            break;
        case KDATE_NUMERIC:
            snprintf(buf, n, "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            break;
        case KDATE_ISO:
            snprintf(buf, n, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            break;
        case KDATE_DAY_MONTH:
        default:
            snprintf(buf, n, "%d %s", t.tm_mday, mon);
            break;
    }
}

// ---------------------------------------------------------------------------
// Pressure
// ---------------------------------------------------------------------------
// The sensor reports hPa and the page has always printed hPa. Millimetres and
// inches of mercury are here because a barometer is the one instrument on this
// page people still read in the units they grew up with, and a figure compared
// against memory has to be in the units that memory is in.
//
// The conversions are the exact definitions: 1 mmHg = 133.322387415 Pa,
// 1 inHg = 3386.389 Pa.
inline float kdPressureValue(float hPa, uint8_t unit) {
    switch (unit) {
        case KPRESS_MMHG: return hPa * 0.750061683f;
        case KPRESS_INHG: return hPa * 0.0295299830f;
        case KPRESS_HPA:
        default:          return hPa;
    }
}

inline const char* kdPressureUnitLabel(uint8_t unit) {
    switch (unit) {
        case KPRESS_MMHG: return "mmHg";
        case KPRESS_INHG: return "inHg";
        case KPRESS_HPA:
        default:          return "hPa";
    }
}

// How many decimals the figure carries. Chosen so each unit resolves about the
// same real change: 1 hPa is 0.75 mmHg and 0.03 inHg, so inches need two
// places to say anything at all, and hectopascals need none.
inline int kdPressureDecimals(uint8_t unit) {
    switch (unit) {
        case KPRESS_MMHG: return 0;
        case KPRESS_INHG: return 2;
        case KPRESS_HPA:
        default:          return 0;
    }
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------
// Applied on the way in from the API and again on the way out to the page.
// Twice, because a config.bin can also arrive by import or from a firmware
// that wrote a field this one has since narrowed, and a stylesheet built from
// an out-of-range enum is a page that renders wrong rather than an error
// somebody sees.
inline void kdSkinClamp(KindleConfig& k) {
    if (k.face > KFACE_CUSTOM)          k.face = KFACE_BOOKERLY;
    if (k.clockStyle > KCLOCK_DATED)    k.clockStyle = KCLOCK_PLAIN;
    if (k.timeFormat > KTIME_12)        k.timeFormat = KTIME_24;
    if (k.dateFormat > KDATE_ISO)       k.dateFormat = KDATE_DAY_MONTH;
    if (k.pressureUnit > KPRESS_INHG)   k.pressureUnit = KPRESS_HPA;
    if (k.tempDecimals > 1)             k.tempDecimals = 1;
    k.boldZones &= 0x01FF;
    k.showFlags &= KSHOW_ALL;
    k.faceCustom[sizeof(k.faceCustom) - 1] = '\0';
}
