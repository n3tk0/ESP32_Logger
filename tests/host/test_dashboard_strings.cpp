// Upper case for the renderer that has no CSS.
//
// WHY THIS IS WORTH A TEST FILE
// -----------------------------
// Every caption on the Kindle dashboard is set uppercase, and on the browser
// page that is one line of CSS — so the string tables and the labels a reader
// types stay in their natural case. The panel has no CSS. It draws whatever
// /kindle/data hands it, which is why one dashboard came out "НАВЪН" in the
// browser and "Навън" on the panel, from one setting on one device.
//
// It cannot be fixed at the reader's end: `tr a-z A-Z` in busybox ash is
// ASCII-only, and a Bulgarian label is exactly the case that needs it.
//
// So the collector uppercases, and the two halves of that are both quiet when
// wrong. Cyrillic in UTF-8 straddles a lead-byte boundary — а-п is D0 B0..BF
// and р-я is D1 80..8F — so the obvious "subtract 0x20" turns "р" into a space
// followed by a capital. And a label that does not fit its buffer has to be
// cut BETWEEN characters: half a two-byte sequence is not a shorter word, it
// is a replacement glyph on a device nobody can attach a console to.
#define FEATURE_KINDLE_DASHBOARD 1

#include "src/web/DashboardStrings.h"
#include "check.h"

#include <string>

static std::string up(const char* s, size_t cap = 128) {
    char buf[256];
    if (cap > sizeof(buf)) cap = sizeof(buf);
    kdUpperUtf8(buf, cap, s);
    return std::string(buf);
}

// The result is kept in a NAMED local before it is compared. CHECK_STREQ takes
// its two arguments into `const char*` on one line and reads them on the next,
// so `CHECK_STREQ(up(x).c_str(), y)` hands it a pointer into a temporary that
// has already been destroyed — which passes for short strings, where the small
// string optimisation leaves the bytes on a stack slot nothing has reused yet,
// and fails for long ones. A test that is itself undefined proves nothing.
static void chk_up(const char* in, const char* want) {
    const std::string got = up(in);
    CHECK_STREQ(got.c_str(), want);
}

// ---------------------------------------------------------------------------
static void test_ascii_is_the_easy_half() {
    chk_up("pressure", "PRESSURE");
    chk_up("Mon", "MON");
    chk_up("august", "AUGUST");
    chk_up("ALREADY", "ALREADY");
    chk_up("", "");
    // Digits, punctuation and the degree pass through: an outlook label is
    // "19:00" and a unit is "hPa" against "°".
    chk_up("19:00", "19:00");
    chk_up("pm2.5 (ug/m3)", "PM2.5 (UG/M3)");
}

// ---------------------------------------------------------------------------
static void test_the_labels_this_was_written_for() {
    chk_up("Навън", "НАВЪН");
    chk_up("Вътре", "ВЪТРЕ");
    chk_up("налягане", "НАЛЯГАНЕ");
    chk_up("септември", "СЕПТЕМВРИ");
    chk_up("точка на оросяване", "ТОЧКА НА ОРОСЯВАНЕ");
    // The short weekday names, which are the seven most repeated strings on
    // the page.
    chk_up("пн", "ПН");
    chk_up("сб", "СБ");
    chk_up("нд", "НД");
}

// ---------------------------------------------------------------------------
// THE ONE THIS FILE EXISTS FOR. The Cyrillic lower case runs across two UTF-8
// lead bytes: а-п live at D0 B0..D0 BF and р-я at D1 80..D1 8F. Treating the
// block as one range and subtracting 0x20 from the trail byte gives, for "р"
// (D1 80), D1 60 — which is not a letter at all.
static void test_the_run_that_straddles_the_lead_byte() {
    // Every letter of the Bulgarian alphabet, in order, lower then upper.
    static const char* LOWER =
        "абвгдежзийклмнопрстуфхцчшщъьюя";
    static const char* UPPER =
        "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЬЮЯ";
    chk_up(LOWER, UPPER);

    // The boundary itself, one letter either side of it.
    chk_up("п", "П");     // D0 BF — last of the first run
    chk_up("р", "Р");     // D1 80 — first of the second
    chk_up("пр", "ПР");

    // Two bytes in, two bytes out: a length that grows would overflow a
    // fixed buffer somewhere downstream.
    CHECK_EQ((long)up(LOWER).size(), (long)strlen(LOWER));
}

// ---------------------------------------------------------------------------
static void test_it_leaves_alone_what_it_does_not_understand() {
    // Not Bulgarian and not ASCII. Copied through rather than mangled: a
    // label is a reader's own text and a byte this does not understand is a
    // byte it must not corrupt.
    chk_up("°C", "°C");
    chk_up("µg/m³", "µG/M³");
    chk_up("→", "→");
    chk_up("日本", "日本");
    // Ё is not Bulgarian but costs two lines, and a Russian label is a
    // plausible thing for somebody to type.
    chk_up("ё", "Ё");
}

// ---------------------------------------------------------------------------
// A cut inside a two-byte sequence is not a shorter word. It is a replacement
// glyph, on a panel on a wall, in a language the person who set it chose.
static void test_it_never_cuts_a_character_in_half() {
    // "НАВЪН" is ten bytes. Every capacity from 1 to 12 has to come back as a
    // whole number of characters and a terminator.
    for (size_t cap = 1; cap <= 12; cap++) {
        char buf[16];
        memset(buf, 0x7E, sizeof(buf));
        kdUpperUtf8(buf, cap, "Навън");
        const size_t n = strlen(buf);
        CHECK(n < cap);                       // always terminated inside cap
        CHECK(n % 2 == 0);                    // whole Cyrillic characters only
        // and nothing was written past the capacity it was given
        CHECK((unsigned char)buf[cap] == 0x7E || cap >= sizeof(buf));
    }

    // The same for a mixed string, where the cut can land on either kind.
    for (size_t cap = 1; cap <= 14; cap++) {
        char buf[16];
        kdUpperUtf8(buf, cap, "aбcдe");
        // Whatever came back is valid UTF-8: no byte is a lone continuation.
        const unsigned char* p = (const unsigned char*)buf;
        while (*p) {
            if (*p < 0x80)      { p++; }
            else if (*p >= 0xC0) { CHECK((p[1] & 0xC0) == 0x80); p += 2; }
            else                 { CHECK(false); break; }   // a lone trail byte
        }
    }
}

// ---------------------------------------------------------------------------
static void test_the_degenerate_calls() {
    char buf[8];
    kdUpperUtf8(buf, sizeof(buf), nullptr);
    CHECK_STREQ(buf, "");
    // A zero capacity has nowhere to put a terminator, so it writes nothing.
    char guard[4] = {0x7E, 0x7E, 0x7E, 0x7E};
    kdUpperUtf8(guard, 0, "Навън");
    CHECK((unsigned char)guard[0] == 0x7E);
    kdUpperUtf8(nullptr, 8, "Навън");        // must not crash
}

int main() {
    RUN(test_ascii_is_the_easy_half);
    RUN(test_the_labels_this_was_written_for);
    RUN(test_the_run_that_straddles_the_lead_byte);
    RUN(test_it_leaves_alone_what_it_does_not_understand);
    RUN(test_it_never_cuts_a_character_in_half);
    RUN(test_the_degenerate_calls);
    return SUMMARY();
}
