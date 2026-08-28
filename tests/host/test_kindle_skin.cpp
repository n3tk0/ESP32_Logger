// ============================================================================
// tests/host/test_kindle_skin.cpp
//
// src/web/KindleSkin.h — the runtime half of the e-ink dashboard's appearance.
//
// WHAT IS WORTH TESTING HERE, AND WHAT IS NOT
// -------------------------------------------
// Not "does the page look right": that is a question for a browser, and
// tools/kindle_preview answers it. What is worth testing is everything that
// turns a stored byte into CSS or into a printed figure, because every one of
// those failures produces a page that renders perfectly and says the wrong
// thing — a clock in the wrong format, a pressure in the wrong unit, a
// stylesheet built from an enum value that no longer exists.
//
//   g++ -std=gnu++17 -Wall -Wextra -O1 -g -I tests/host/shims -I.
//       tests/host/test_kindle_skin.cpp -o t && ./t
// ============================================================================
// Compiled as a build that HAS the dashboard, so kdFmtDate() reaches the real
// month table in DashboardStrings.h. Without this it would fall back to an
// empty month name and the date assertions would pass on nothing.
#define FEATURE_KINDLE_DASHBOARD 1

#include <Arduino.h>
#include <string>
#include <cstring>

#include "src/web/KindleSkin.h"
#include "check.h"

// A sink with the two operators kdSkinCss() needs. The firmware passes an
// Arduino String; the desktop shim has no operator+=(int), and inventing one
// there to serve one test would be putting a firmware behaviour into a file
// whose whole job is not to pretend to be firmware.
struct Css {
    std::string s;
    Css& operator+=(const char* c) { s += c;                 return *this; }
    Css& operator+=(int n)         { s += std::to_string(n); return *this; }
    bool has(const char* needle) const { return s.find(needle) != std::string::npos; }
};

static KindleConfig defaults() {
    KindleConfig k{};
    k.face = KFACE_BOOKERLY;
    k.faceCustom[0] = '\0';
    k.boldZones = 0;
    k.showFlags = KSHOW_ALL;
    k.clockStyle = KCLOCK_PLAIN;
    k.timeFormat = KTIME_24;
    k.dateFormat = KDATE_DAY_MONTH;
    k.pressureUnit = KPRESS_HPA;
    k.tempDecimals = 1;
    return k;
}

static struct tm at(int year, int mon, int mday, int hour, int min) {
    struct tm t{};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    return t;
}

// ---------------------------------------------------------------------------
static void test_defaults_emit_nothing() {
    Css css;
    kdSkinCss(css, defaults());
    // The point of the whole design: a device that has never visited the
    // settings page must render exactly what the base stylesheet says, and pay
    // nothing for the fact that it could have been changed.
    CHECK(css.s.empty());
    CHECK(kdSkinIsDefault(defaults()));
}

static void test_face_reaches_the_chart_too() {
    KindleConfig k = defaults();
    k.face = KFACE_HELVETICA;
    Css css;
    kdSkinCss(css, k);
    CHECK(css.has("Helvetica"));
    // .ax is the chart's axis labels, which are SVG text. SVG text does not
    // inherit font-family from an HTML ancestor in every engine this page may
    // meet, so leaving it out gives a chart in the old face on a page in the
    // new one.
    CHECK(css.has("body,.ax{font-family:"));
    // Every stack ends in a generic family: a face the reader does not carry
    // must fall back to something rather than to nothing.
    CHECK(css.has("sans-serif}"));
}

static void test_every_face_has_a_generic_fallback() {
    // Which faces a reader can reach depends on its firmware. A stack that
    // resolves to nothing renders in the browser's default, which is a page
    // that ignored the setting; every named stack has to end somewhere real.
    for (uint8_t f = KFACE_BOOKERLY; f <= KFACE_FUTURA; f++) {
        KindleConfig k = defaults();
        k.face = f;
        const char* stack = kdFaceStack(k);
        CHECK(stack != nullptr);
        if (!stack) continue;
        const std::string s(stack);
        CHECK(s.size() > 6 &&
              s.compare(s.size() - 5, 5, "serif") == 0);   // "serif" or "sans-serif"
    }
    // The custom face is the one that returns nothing, because its text lives
    // in faceCustom and only the caller knows whether it is empty.
    KindleConfig custom = defaults();
    custom.face = KFACE_CUSTOM;
    CHECK(kdFaceStack(custom) == nullptr);
}

static void test_custom_face_is_used_verbatim_or_not_at_all() {
    KindleConfig k = defaults();
    k.face = KFACE_CUSTOM;
    strcpy(k.faceCustom, "Amazon Ember,serif");
    Css css;
    kdSkinCss(css, k);
    CHECK(css.has("Amazon Ember,serif"));

    // An empty custom list must produce no font-family rule at all rather than
    // "font-family:", which is a broken declaration that takes the rest of the
    // block with it in some parsers.
    KindleConfig e = defaults();
    e.face = KFACE_CUSTOM;
    e.faceCustom[0] = '\0';
    Css none;
    kdSkinCss(none, e);
    CHECK(!none.has("font-family"));
}

static void test_bold_zones_are_independent() {
    KindleConfig k = defaults();
    k.boldZones = KBOLD_PRESSURE;
    Css css;
    kdSkinCss(css, k);
    CHECK(css.has(".pres{font-weight:bold}"));
    // The neighbouring bits must not have come along: a mask bug here shows up
    // as a page where the wrong number is heavy, which reads as a design
    // choice rather than as a fault.
    CHECK(!css.has(".big{font-weight"));
    CHECK(!css.has(".clock{font-weight"));

    KindleConfig all = defaults();
    all.boldZones = 0x01FF;
    Css every;
    kdSkinCss(every, all);
    size_t n = 0;
    for (size_t i = every.s.find("font-weight"); i != std::string::npos;
         i = every.s.find("font-weight", i + 1)) n++;
    CHECK(n == 9);
}

static void test_clock_styles_keep_their_height() {
    // Each style is allowed to change how the clock looks; none of them is
    // allowed to change how tall the block is, because that height is what
    // keeps the hairline under it level with the outdoor column. The browser
    // check lives in tools/kindle_preview; this one is the arithmetic.
    KindleConfig boxed = defaults();  boxed.clockStyle = KCLOCK_BOXED;
    Css b; kdSkinCss(b, boxed);
    CHECK(b.has("background:#000"));
    CHECK(b.has("height:131px"));       // 131 + 8 margin = the 139 of the design
    CHECK(b.has("margin-bottom:8px"));

    KindleConfig dated = defaults();  dated.clockStyle = KCLOCK_DATED;
    Css d; kdSkinCss(d, dated);
    CHECK(d.has("height:112px"));       // 112 + 27 = 139: taken from the clock,
    CHECK(d.has(".clock-d{height:27px")); // not added underneath it

    KindleConfig ruled = defaults();  ruled.clockStyle = KCLOCK_RULED;
    Css r; kdSkinCss(r, ruled);
    CHECK(r.has("border-top:1px solid #000"));
    // box-sizing is border-box for the whole page, so the rule comes out of the
    // padding and the height is not restated at all.
    CHECK(!r.has("height:"));
}

// ---------------------------------------------------------------------------
static void test_time_formats() {
    char buf[16];
    const struct tm morning = at(2026, 8, 27, 9, 5);
    const struct tm evening = at(2026, 8, 27, 17, 40);
    const struct tm midnight = at(2026, 8, 27, 0, 0);
    const struct tm noon = at(2026, 8, 27, 12, 0);

    kdFmtTime(buf, sizeof(buf), morning, KTIME_24);      CHECK(!strcmp(buf, "09:05"));
    kdFmtTime(buf, sizeof(buf), morning, KTIME_24_LEAN); CHECK(!strcmp(buf, "9:05"));
    kdFmtTime(buf, sizeof(buf), morning, KTIME_12);      CHECK(!strcmp(buf, "9:05am"));
    kdFmtTime(buf, sizeof(buf), evening, KTIME_12);      CHECK(!strcmp(buf, "5:40pm"));

    // The two hours a 12-hour clock gets wrong when it is written with %, and
    // the reason this test exists: hour 0 is 12am, not 0am, and hour 12 is
    // 12pm, not 0pm.
    kdFmtTime(buf, sizeof(buf), midnight, KTIME_12);     CHECK(!strcmp(buf, "12:00am"));
    kdFmtTime(buf, sizeof(buf), noon, KTIME_12);         CHECK(!strcmp(buf, "12:00pm"));
    kdFmtTime(buf, sizeof(buf), midnight, KTIME_24);     CHECK(!strcmp(buf, "00:00"));
}

static void test_date_formats() {
    char buf[32];
    const struct tm day = at(2026, 8, 27, 9, 5);

    kdFmtDate(buf, sizeof(buf), day, KDATE_NUMERIC); CHECK(!strcmp(buf, "27.08.2026"));
    kdFmtDate(buf, sizeof(buf), day, KDATE_ISO);     CHECK(!strcmp(buf, "2026-08-27"));

    // The month name comes from DashboardStrings.h and depends on the build
    // language, so the assertion is that there IS one and which side of the
    // number it falls on, rather than on the word itself.
    kdFmtDate(buf, sizeof(buf), day, KDATE_DAY_MONTH);
    CHECK(!strncmp(buf, "27 ", 3));
    CHECK(strlen(buf) > 4);                       // a month name, not a blank
    kdFmtDate(buf, sizeof(buf), day, KDATE_MONTH_DAY);
    CHECK(strlen(buf) > 4 && buf[0] != ' ');      // month first, and not empty
    CHECK(!strcmp(buf + strlen(buf) - 3, " 27"));

    // Out-of-range months come from a config nobody validated. kdMonth()
    // clamps rather than indexing past its table.
    struct tm bad = day;
    bad.tm_mon = 99;
    kdFmtDate(buf, sizeof(buf), bad, KDATE_DAY_MONTH);
    CHECK(strlen(buf) > 4);
}

static void test_pressure_units() {
    // Standard atmosphere: 1013.25 hPa is 760 mmHg and 29.92 inHg. If the
    // conversion is wrong these are the numbers everybody knows.
    CHECK(kdPressureValue(1013.25f, KPRESS_HPA) == 1013.25f);
    const float mm = kdPressureValue(1013.25f, KPRESS_MMHG);
    CHECK(mm > 759.9f && mm < 760.1f);
    const float in = kdPressureValue(1013.25f, KPRESS_INHG);
    CHECK(in > 29.91f && in < 29.93f);

    // Inches need two places to say anything: one hPa is 0.03 inHg, so a
    // whole-number inHg reading would sit unchanged through a whole storm.
    CHECK(kdPressureDecimals(KPRESS_INHG) == 2);
    CHECK(kdPressureDecimals(KPRESS_HPA) == 0);
    CHECK(!strcmp(kdPressureUnitLabel(KPRESS_MMHG), "mmHg"));

    // Negative deltas convert too — the three-hour change is signed and is
    // printed in the same unit as the figure above it.
    CHECK(kdPressureValue(-1.2f, KPRESS_MMHG) < 0.0f);
}

// ---------------------------------------------------------------------------
static void test_clamp_rejects_what_a_form_cannot_send() {
    // A config.bin can also arrive by settings import, or be written by a
    // firmware with one more clock style than this one. A stylesheet built
    // from an enum value this build does not have is a page that renders
    // wrong rather than an error somebody sees.
    KindleConfig k{};
    k.face = 200;
    k.clockStyle = 99;
    k.timeFormat = 7;
    k.dateFormat = 7;
    k.pressureUnit = 7;
    k.tempDecimals = 5;
    k.boldZones = 0xFFFF;
    k.showFlags = 0xFFFF;
    memset(k.faceCustom, 'x', sizeof(k.faceCustom));   // deliberately unterminated

    kdSkinClamp(k);

    CHECK(k.face == KFACE_BOOKERLY);
    CHECK(k.clockStyle == KCLOCK_PLAIN);
    CHECK(k.timeFormat == KTIME_24);
    CHECK(k.dateFormat == KDATE_DAY_MONTH);
    CHECK(k.pressureUnit == KPRESS_HPA);
    CHECK(k.tempDecimals == 1);
    CHECK(k.boldZones == 0x01FF);        // the nine bits that exist
    CHECK(k.showFlags == KSHOW_ALL);
    CHECK(strlen(k.faceCustom) == sizeof(k.faceCustom) - 1);   // terminated

    // A clamped-to-defaults config still renders as the design.
    KindleConfig ok = defaults();
    kdSkinClamp(ok);
    Css css;
    kdSkinCss(css, ok);
    CHECK(css.s.empty());
}

static void test_clamp_leaves_a_valid_config_alone() {
    KindleConfig k = defaults();
    k.face = KFACE_FUTURA;
    k.clockStyle = KCLOCK_DATED;
    k.timeFormat = KTIME_12;
    k.dateFormat = KDATE_ISO;
    k.pressureUnit = KPRESS_INHG;
    k.tempDecimals = 0;
    k.boldZones = KBOLD_CLOCK | KBOLD_WEEK;
    k.showFlags = KSHOW_ALL & ~KSHOW_WEEK;
    const KindleConfig before = k;
    kdSkinClamp(k);
    CHECK(memcmp(&before, &k, sizeof(k)) == 0);
}

int main() {
    RUN(test_defaults_emit_nothing);
    RUN(test_face_reaches_the_chart_too);
    RUN(test_every_face_has_a_generic_fallback);
    RUN(test_custom_face_is_used_verbatim_or_not_at_all);
    RUN(test_bold_zones_are_independent);
    RUN(test_clock_styles_keep_their_height);
    RUN(test_time_formats);
    RUN(test_date_formats);
    RUN(test_pressure_units);
    RUN(test_clamp_rejects_what_a_form_cannot_send);
    RUN(test_clamp_leaves_a_valid_config_alone);
    return SUMMARY();
}
