// ============================================================================
// src/web/DashboardStrings.h
//
// Dashboard wording in English or Bulgarian, chosen at build time.
//
//   -DKINDLE_LANG_BG    Bulgarian
//   (default)           English
//
// A compile-time switch rather than a runtime setting: the unused literal is
// discarded by the compiler, so a single-language build pays nothing for the
// other. The page is served to one reader on one shelf; nobody needs to
// change its language without a reflash.
//
// WHY THE NAMES ARE TABLES AND NOT strftime
// -----------------------------------------
// strftime("%a") would give English names from the C locale, and the ESP32's
// newlib has no bg_BG locale to switch to — setlocale() there accepts only
// "C". So the weekday names live here.
//
// The long weekday names that used to sit alongside these went with the
// masthead; `git log` has them if a date line ever wants a home again.
//
// CYRILLIC ON THE TARGET
// ----------------------
// The page declares UTF-8 and the device's serif faces are named first, but
// Bookerly's Cyrillic coverage varies by firmware. The stack therefore falls
// through to the reader's own fallback; if a Bulgarian build renders boxes,
// that is the font, not the encoding, and the fix is to pick a face the device
// definitely carries — Settings → E-ink dashboard, which is a runtime setting
// (config.kindle.face) and not a reflash.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_KINDLE_DASHBOARD

// Pick one of a pair. Both literals are written out, only one is emitted.
#if defined(KINDLE_LANG_BG)
#  define KD_T(en, bg) bg
#else
#  define KD_T(en, bg) en
#endif

// Monday-based index, for the week strip.
inline const char* kdWeekdayShort(int mondayIdx) {
    static const char* N[7] = {
#if defined(KINDLE_LANG_BG)
        "пн", "вт", "ср", "чт", "пт", "сб", "нд"
#else
        "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
#endif
    };
    return N[(mondayIdx < 0 || mondayIdx > 6) ? 0 : mondayIdx];
}

// tm_mon counts from January. Set uppercase by CSS, so these stay lower case —
// Bulgarian month names are not capitalised in running text, and text-transform
// is what makes the section heading match the two above it.
inline const char* kdMonth(int mon) {
    static const char* N[12] = {
#if defined(KINDLE_LANG_BG)
        "януари", "февруари", "март", "април", "май", "юни",
        "юли", "август", "септември", "октомври", "ноември", "декември"
#else
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
#endif
    };
    return N[(mon < 0 || mon > 11) ? 0 : mon];
}

// Weekday abbreviation for a forecast column, N days ahead of `wday`.
inline const char* kdWeekdayAhead(int wday, int daysAhead) {
    static const char* N[7] = {
#if defined(KINDLE_LANG_BG)
        "нд", "пн", "вт", "ср", "чт", "пт", "сб"
#else
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
#endif
    };
    return N[(wday + daysAhead) % 7];
}

#endif  // FEATURE_KINDLE_DASHBOARD
