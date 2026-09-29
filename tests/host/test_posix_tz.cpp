// Host unit tests for src/utils/PosixTz.h
//   - the TZ string for each daylight-saving rule
//   - that the C library reading those strings changes the clock at the right
//     instant: the EU at 01:00 UTC on the last Sunday of March and October,
//     the US at 02:00 local on the second Sunday of March / first of November
//   - tzOffsetAt() across a day and a year boundary
//
// glibc stands in for newlib here: both implement the same POSIX TZ grammar,
// and the strings deliberately use nothing beyond it.
#include <stdlib.h>
#include <time.h>

#include "src/utils/PosixTz.h"
#include "check.h"

static void useTz(int tz, uint8_t rule, int manual = 0) {
    char buf[48];
    buildPosixTz(tz, rule, manual, buf, sizeof(buf));
    setenv("TZ", buf, 1);
    tzset();
}

// ---------------------------------------------------------------------------
static void test_strings() {
    char b[48];
    buildPosixTz(2, DST_RULE_EU, 0, b, sizeof(b));   CHECK_STREQ(b, "STD-2DST,M3.5.0/3,M10.5.0/4");
    buildPosixTz(1, DST_RULE_EU, 0, b, sizeof(b));   CHECK_STREQ(b, "STD-1DST,M3.5.0/2,M10.5.0/3");
    buildPosixTz(0, DST_RULE_EU, 0, b, sizeof(b));   CHECK_STREQ(b, "STD0DST,M3.5.0/1,M10.5.0/2");
    buildPosixTz(-5, DST_RULE_US, 0, b, sizeof(b));  CHECK_STREQ(b, "STD5DST,M3.2.0/2,M11.1.0/2");
    buildPosixTz(2, DST_RULE_OFF, 1, b, sizeof(b));  CHECK_STREQ(b, "STD-2");
    buildPosixTz(2, DST_RULE_MANUAL, 1, b, sizeof(b)); CHECK_STREQ(b, "STD-3");
    // A manual rule saved with no hours still means "one hour ahead".
    buildPosixTz(2, DST_RULE_MANUAL, 0, b, sizeof(b)); CHECK_STREQ(b, "STD-3");
    // A byte from a newer firmware falls back to the EU rule, not to garbage.
    buildPosixTz(2, 77, 0, b, sizeof(b));            CHECK_STREQ(b, "STD-2DST,M3.5.0/3,M10.5.0/4");
    // Never negative rule times: newlib parses them unsigned.
    buildPosixTz(-5, DST_RULE_EU, 0, b, sizeof(b));  CHECK_STREQ(b, "STD5DST,M3.5.0/0,M10.5.0/0");
}

// ---------------------------------------------------------------------------
static void test_eu_transitions() {
    useTz(2, DST_RULE_EU);
    const time_t start = 1774746000;   // 2026-03-29 01:00 UTC
    const time_t end   = 1792890000;   // 2026-10-25 01:00 UTC
    CHECK_EQ(tzOffsetAt(start - 1), 7200);
    CHECK_EQ(tzOffsetAt(start),     10800);
    CHECK_EQ(tzOffsetAt(end - 1),   10800);
    CHECK_EQ(tzOffsetAt(end),       7200);
    struct tm lt;
    localtime_r(&start, &lt);
    CHECK_EQ(lt.tm_hour, 4);            // 03:00 EET became 04:00 EEST
    CHECK_EQ(lt.tm_isdst, 1);

    // Central Europe changes at the same instant.
    useTz(1, DST_RULE_EU);
    CHECK_EQ(tzOffsetAt(start - 1), 3600);
    CHECK_EQ(tzOffsetAt(start),     7200);
    CHECK_EQ(tzOffsetAt(end),       3600);
    // West of UTC-1 the rule times are held at 00:00 local (newlib reads
    // them unsigned); the settings page's preview uses the same instants.
    useTz(-5, DST_RULE_EU);
    CHECK_EQ(tzOffsetAt(1774760400 - 1), -18000);  // 2026-03-29 05:00 UTC
    CHECK_EQ(tzOffsetAt(1774760400),     -14400);
    CHECK_EQ(tzOffsetAt(1792900800 - 1), -14400);  // 2026-10-25 04:00 UTC
    CHECK_EQ(tzOffsetAt(1792900800),     -18000);
}

static void test_us_transitions() {
    useTz(-5, DST_RULE_US);
    const time_t start = 1772953200;   // 2026-03-08 07:00 UTC = 02:00 EST
    const time_t end   = 1793512800;   // 2026-11-01 06:00 UTC = 02:00 EDT
    CHECK_EQ(tzOffsetAt(start - 1), -18000);
    CHECK_EQ(tzOffsetAt(start),     -14400);
    CHECK_EQ(tzOffsetAt(end - 1),   -14400);
    CHECK_EQ(tzOffsetAt(end),       -18000);
}

static void test_fixed_rules() {
    const time_t summer = 1784000000;  // July 2026
    useTz(2, DST_RULE_OFF);
    CHECK_EQ(tzOffsetAt(summer), 7200);
    useTz(2, DST_RULE_MANUAL, 1);
    CHECK_EQ(tzOffsetAt(summer), 10800);
    CHECK_EQ(tzOffsetAt(1798759800), 10800);   // and in winter
}

// Local and UTC on different days, and on different years.
static void test_offset_across_boundaries() {
    const time_t nye = 1798759800;     // 2026-12-31 23:30 UTC
    useTz(2, DST_RULE_OFF);
    CHECK_EQ(tzOffsetAt(nye), 7200);   // local is already 2027
    useTz(-5, DST_RULE_OFF);
    CHECK_EQ(tzOffsetAt(nye + 3600), -18000);   // UTC is 2027, local is not
    useTz(14, DST_RULE_OFF);
    CHECK_EQ(tzOffsetAt(nye), 14 * 3600);
    useTz(-12, DST_RULE_OFF);
    CHECK_EQ(tzOffsetAt(nye), -12 * 3600);
}

int main() {
    RUN(test_strings);
    RUN(test_eu_transitions);
    RUN(test_us_transitions);
    RUN(test_fixed_rules);
    RUN(test_offset_across_boundaries);
    return SUMMARY();
}
