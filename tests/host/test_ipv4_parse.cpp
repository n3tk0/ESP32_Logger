// Host unit tests for src/utils/Ipv4Parse.h — the dotted-quad parser that
// replaced sscanf in the settings form (WebServer.cpp applyNetworkForm) and
// WiFiModule's static-IP setup.
//
// The stricter behaviour is the point: sscanf("%hhu") took "300" as 44 and
// "%d" let trailing junk through, so a typo in the static IP field could
// silently put the collector on the wrong address.
#include <stdint.h>

#include "src/utils/Ipv4Parse.h"
#include "check.h"

static bool is(const uint8_t o[4], int a, int b, int c, int d) {
    return o[0] == a && o[1] == b && o[2] == c && o[3] == d;
}

static void test_valid_quads() {
    uint8_t o[4] = { 0, 0, 0, 0 };
    CHECK(ipv4Parse("192.168.1.50", o));
    CHECK(is(o, 192, 168, 1, 50));
    CHECK(ipv4Parse("0.0.0.0", o));
    CHECK(is(o, 0, 0, 0, 0));
    CHECK(ipv4Parse("255.255.255.255", o));
    CHECK(is(o, 255, 255, 255, 255));
    CHECK(ipv4Parse("  10.0.0.1 ", o));               // a pasted value's spaces
    CHECK(is(o, 10, 0, 0, 1));
    CHECK(ipv4Parse("010.001.0.9", o));               // leading zeros are decimal
    CHECK(is(o, 10, 1, 0, 9));
}

static void test_refused_and_untouched() {
    const char* bad[] = { "300.1.1.1", "1.2.3.256", "1.2.3", "1.2.3.4.5", "1.2.3.4junk",
                          "1..2.3", ".1.2.3", "1.2.3.", "a.b.c.d", "", "   ", "1.2.3.0004",
                          "1 .2.3.4", "-1.2.3.4", "1.2.3.4/24" };
    for (const char* s : bad) {
        uint8_t o[4] = { 7, 7, 7, 7 };
        const bool ok = ipv4Parse(s, o);
        if (ok) std::printf("  accepted \"%s\"\n", s);
        CHECK(!ok);
        CHECK(is(o, 7, 7, 7, 7));
    }
    uint8_t o[4] = { 7, 7, 7, 7 };
    CHECK(!ipv4Parse(nullptr, o));
    CHECK(is(o, 7, 7, 7, 7));
}

int main() {
    RUN(test_valid_quads);
    RUN(test_refused_and_untouched);
    return SUMMARY();
}
