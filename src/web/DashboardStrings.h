// ============================================================================
// src/web/DashboardStrings.h
//
// Dashboard wording in English or Bulgarian, chosen at RUN time.
//
//   Settings -> E-ink dashboard -> Language, or config.kindle.lang
//   -DKINDLE_LANG_BG    what that setting defaults to
//
// It used to be the build flag alone, on the argument that a single-language
// build pays nothing for the other and nobody needs to change the language
// without a reflash. The second half of that was wrong: the device is a panel
// on a wall, its reader is not the person who built the firmware, and "reflash
// to read it in your own language" is not an answer. Both literals are
// compiled in now — under a kilobyte of flash across the whole page, and
// nothing of RAM — and which one is read is a setting, for the browser page
// and the FBInk panel together.
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

#include <stdint.h>
#include <stddef.h>          // size_t, for kdUpperUtf8()

// ---------------------------------------------------------------------------
// Which language the dashboard is written in
// ---------------------------------------------------------------------------
// The browser page and the FBInk panel together, because they are one design
// rendered twice and nobody reads one in English and the other in Bulgarian.
//
// Stored as config.kindle.lang. OUTSIDE the feature guard below, because
// Config.h's struct carries the field whether or not the dashboard is compiled
// in, and a name that exists only in some builds is a name that breaks one.
//
// KLANG_AUTO IS ZERO, AND THAT IS THE POINT. It is what an older config's
// reserved byte reads as, so a device upgrading into this keeps saying
// whatever its firmware was built to say — the compile-time -DKINDLE_LANG_BG,
// which is now the default rather than the decision. Choosing either of the
// other two overrides it, and needs no reflash.
enum KindleLang : uint8_t {
    KLANG_AUTO = 0,     ///< as the firmware was built
    KLANG_EN   = 1,
    KLANG_BG   = 2
};

#ifdef FEATURE_KINDLE_DASHBOARD

// ---------------------------------------------------------------------------
// Which language, and when it is decided
// ---------------------------------------------------------------------------
//
// IT USED TO BE THE COMPILER'S DECISION. -DKINDLE_LANG_BG picked one literal of
// each pair and discarded the other, which cost nothing and could not be
// changed without a reflash — on a device whose whole point is that it hangs on
// a wall. A reader who wanted the other language had to build firmware.
//
// It is a setting now (config.kindle.lang), and the build flag is what that
// setting defaults to. Both literals are compiled in; the pair costs a pointer
// and the strings themselves, which on a page of about forty is under a
// kilobyte of flash and nothing at all of RAM.
//
// ONE VARIABLE, NOT A PARAMETER THREADED THROUGH THIRTY FUNCTIONS. kdT() is
// called from the tendency wording, from a metric's label, from the middle of
// the chart — none of which has a KindleConfig to consult, and giving them all
// one would be a parameter that exists to be passed on. The current language is
// genuinely ambient: it is a property of the page being rendered, and every
// page is rendered start to finish on the async web server's own task, so
// nothing else is looking while it is set. kdLangBegin() is called once at the
// top of each handler and says so out loud.
//
// The function-local static is what lets this stay header-only: C++17 gives one
// instance across every translation unit that includes this, so a host test can
// include the header on its own and link.
inline uint8_t& kdLangRef() {
#if defined(KINDLE_LANG_BG)
    static uint8_t v = KLANG_BG;
#else
    static uint8_t v = KLANG_EN;
#endif
    return v;
}

/// Turn the stored setting into the language actually in use.
///
/// KLANG_AUTO — which is what an older config's reserved byte reads as, and
/// therefore what every device upgrading into this has — means "as the
/// firmware was built". Anything unrecognised means the same: a byte from
/// storage is not a promise.
inline uint8_t kdLangResolve(uint8_t stored) {
    if (stored == KLANG_EN || stored == KLANG_BG) return stored;
#if defined(KINDLE_LANG_BG)
    return KLANG_BG;
#else
    return KLANG_EN;
#endif
}

/// Set the language for the page about to be rendered. Call once, at the top.
inline void kdLangBegin(uint8_t stored) { kdLangRef() = kdLangResolve(stored); }

inline bool kdLangIsBg() { return kdLangRef() == KLANG_BG; }

/// Pick one of a pair. Both are compiled in; which one is read is a setting.
inline const char* kdT(const char* en, const char* bg) {
    return kdLangIsBg() ? bg : en;
}

// A macro still, so the call sites read the same as they always did — and so
// that KindleSlots.h's `#ifndef KD_T` fallback keeps working for a host test
// that includes it on its own.
//
// IT IS NO LONGER A CONSTANT, and the one thing that changes is that it can no
// longer be pasted between string literals: `"a" KD_T("b","c") "d"` was
// compile-time concatenation and is now a syntax error, which is a good way for
// this to fail rather than a bad one.
#define KD_T(en, bg) kdT((en), (bg))

// ---------------------------------------------------------------------------
// Upper case, for the renderer that has no CSS
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS
// ---------------
// Every caption on this dashboard is set uppercase — the group headings, the
// cell labels, the section rules, the weekday names, the month heading. On the
// browser page that is one line of CSS (`text-transform:uppercase`) and the
// strings themselves stay in their natural case, which is why the tables above
// are lower case and the note over kdMonth() says so.
//
// The Kindle's own renderer has no CSS. It draws whatever string /kindle/data
// hands it, so the SAME dashboard came out "НАВЪН" and "MON" in the browser and
// "Навън" and "Mon" on the panel — from one setting, on one device, and most
// visibly on the two strings a reader chose themselves.
//
// It cannot be fixed at the other end. `tr a-z A-Z` in the reader's busybox ash
// is ASCII-only, and a Bulgarian label is exactly the case that needs it: the
// panel would keep drawing "Навън" while uppercasing "Pressure". So the
// collector sends the panel a string that is already in the case the design
// asks for.
//
// WHAT IT COVERS, AND WHAT IT DELIBERATELY DOES NOT
// -------------------------------------------------
// ASCII and Cyrillic, which is what this firmware's two languages are written
// in. Everything else is copied through untouched — a byte this does not
// understand is a byte it must not corrupt, and a label is a reader's own text.
//
// Cyrillic in UTF-8 is two bytes, and the lower-case letters sit in two runs:
//   а-п  D0 B0..D0 BF  ->  А-П  D0 90..D0 9F   (subtract 0x20 from the second)
//   р-я  D1 80..D1 8F  ->  Р-Я  D0 A0..D0 AF   (lead D1->D0, add 0x20)
// The two runs are why this is not one subtraction: the block straddles a
// UTF-8 lead-byte boundary, and treating it as one range turns "р" into " Р".
// Ё (D1 91 -> D0 81) is handled with them; it is not Bulgarian but it costs
// two lines and a Russian label is a plausible thing for someone to type.
//
// Truncation is on a character, never inside one: half a two-byte sequence is
// not a shorter word, it is a replacement glyph.
inline void kdUpperUtf8(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) return;
    dst[0] = '\0';
    if (src == nullptr) return;

    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p; ) {
        // How many bytes this character occupies, and what it becomes.
        unsigned char out[4];
        size_t n = 0;

        if (*p < 0x80) {
            out[n++] = (*p >= 'a' && *p <= 'z') ? (unsigned char)(*p - 32) : *p;
            p++;
        } else if (*p == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) {
            out[n++] = 0xD0; out[n++] = (unsigned char)(p[1] - 0x20);   // а-п
            p += 2;
        } else if (*p == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) {
            out[n++] = 0xD0; out[n++] = (unsigned char)(p[1] + 0x20);   // р-я
            p += 2;
        } else if (*p == 0xD1 && p[1] == 0x91) {
            out[n++] = 0xD0; out[n++] = 0x81;                           // ё
            p += 2;
        } else {
            // Anything else, including a malformed sequence: copied byte for
            // byte. A lead byte says how long it claims to be, and a truncated
            // string is cut before it rather than inside it.
            const unsigned char c = *p;
            size_t len = (c < 0xC0) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            for (size_t i = 0; i < len && p[i]; i++) out[n++] = p[i];
            p += n;
        }

        if (o + n + 1 > cap) break;      // stops on a character boundary
        for (size_t i = 0; i < n; i++) dst[o++] = (char)out[i];
    }
    dst[o] = '\0';
}

// Monday-based index, for the week strip.
inline const char* kdWeekdayShort(int mondayIdx) {
    static const char* const EN[7] = { "Mon", "Tue", "Wed", "Thu",
                                       "Fri", "Sat", "Sun" };
    static const char* const BG[7] = { "пн", "вт", "ср", "чт",
                                       "пт", "сб", "нд" };
    const int i = (mondayIdx < 0 || mondayIdx > 6) ? 0 : mondayIdx;
    return kdLangIsBg() ? BG[i] : EN[i];
}

// tm_mon counts from January. Set uppercase by CSS, so these stay lower case —
// Bulgarian month names are not capitalised in running text, and text-transform
// is what makes the section heading match the two above it.
inline const char* kdMonth(int mon) {
    static const char* const EN[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    static const char* const BG[12] = {
        "януари", "февруари", "март", "април", "май", "юни",
        "юли", "август", "септември", "октомври", "ноември", "декември" };
    const int i = (mon < 0 || mon > 11) ? 0 : mon;
    return kdLangIsBg() ? BG[i] : EN[i];
}

// Weekday abbreviation for a forecast column, N days ahead of `wday`.
inline const char* kdWeekdayAhead(int wday, int daysAhead) {
    static const char* const EN[7] = { "Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat" };
    static const char* const BG[7] = { "нд", "пн", "вт", "ср",
                                       "чт", "пт", "сб" };
    // CLAMPED LIKE ITS TWO SIBLINGS. `%` on a negative sum is negative in C,
    // so an unguarded index reads before the array — and the one caller that
    // now passes a STORED value, forecastPeriodLabel() with Period::wday,
    // holds -1 for an hourly column. Its own `wday >= 0` test was the only
    // thing standing between that and EN[-1].
    int i = (wday + daysAhead) % 7;
    if (i < 0) i += 7;
    if (i < 0 || i > 6) i = 0;
    return kdLangIsBg() ? BG[i] : EN[i];
}

#endif  // FEATURE_KINDLE_DASHBOARD
