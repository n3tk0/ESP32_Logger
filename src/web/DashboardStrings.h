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

#include <stddef.h>          // size_t, for kdUpperUtf8()

#ifdef FEATURE_KINDLE_DASHBOARD

// Pick one of a pair. Both literals are written out, only one is emitted.
#if defined(KINDLE_LANG_BG)
#  define KD_T(en, bg) bg
#else
#  define KD_T(en, bg) en
#endif

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
