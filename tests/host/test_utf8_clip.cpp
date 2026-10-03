// Host unit tests for src/utils/Utf8Clip.h — a name cut to a fixed buffer must
// not end in half a letter, or a browser round trip changes it (Utf8Clip.h).
#include <string.h>

#include "src/utils/Utf8Clip.h"
#include "check.h"

static bool clipsTo(const char* in, const char* want) {
    char b[32];
    strcpy(b, in);
    utf8ClipTail(b);
    if (strcmp(b, want) != 0) std::printf("  \"%s\" -> \"%s\"\n", in, b);
    return strcmp(b, want) == 0;
}

static void test_whole_names_stay() {
    CHECK(clipsTo("", ""));
    CHECK(clipsTo("env_indoor", "env_indoor"));
    CHECK(clipsTo("Вътрешен", "Вътрешен"));         // exactly 16 bytes
    CHECK(clipsTo("t\xE2\x82\xAC", "t\xE2\x82\xAC")); // a whole 3-byte sign
    CHECK(clipsTo("\xF0\x9F\x8C\xA1", "\xF0\x9F\x8C\xA1"));
}

static void test_half_letters_go() {
    // "Външен сензор" cut at 16 bytes: half of "е".
    char id[17];
    strncpy(id, "Външен сензор", sizeof(id) - 1);
    id[16] = '\0';
    utf8ClipTail(id);
    CHECK(strcmp(id, "Външен с") == 0);
    // ...and what the page saved back: U+FFFD cut to its first byte, or two.
    CHECK(clipsTo("Външен с\xEF", "Външен с"));
    CHECK(clipsTo("Външен с\xEF\xBF", "Външен с"));
    CHECK(clipsTo("x\xF0\x9F\x8C", "x"));
}

static void test_stray_bytes_left() {
    CHECK(clipsTo("\x80\x80", "\x80\x80"));         // nothing to cut back to
    CHECK(clipsTo("a\x80", "a\x80"));               // not a cut letter
}

int main() {
    RUN(test_whole_names_stay);
    RUN(test_half_letters_go);
    RUN(test_stray_bytes_left);
    return SUMMARY();
}
