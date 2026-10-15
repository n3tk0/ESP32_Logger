// Host tests for the 429 back-off (src/export/Throttle.h).
#include "src/export/Throttle.h"
#include "check.h"

static void test_backoff_doubles_to_cap() {
    uint32_t b = 0;
    const uint32_t want[] = {60000, 120000, 240000, 480000, 900000, 900000};
    for (uint32_t w : want) {
        b = throttleNextBackoffMs(b);
        CHECK_EQ(b, w);
    }
}

static void test_retry_after_wins_only_when_longer() {
    CHECK_EQ(throttleWaitMs(60000, 0),   (uint32_t)60000);
    CHECK_EQ(throttleWaitMs(60000, 30),  (uint32_t)60000);
    CHECK_EQ(throttleWaitMs(60000, 300), (uint32_t)300000);
    // Capped at an hour.
    CHECK_EQ(throttleWaitMs(60000, 999999), (uint32_t)3600000);
}

static void test_parse_retry_after() {
    CHECK_EQ(parseRetryAfterS("120"),  (uint32_t)120);
    CHECK_EQ(parseRetryAfterS(" 5 "),  (uint32_t)5);
    CHECK_EQ(parseRetryAfterS(""),     (uint32_t)0);
    CHECK_EQ(parseRetryAfterS(nullptr), (uint32_t)0);
    CHECK_EQ(parseRetryAfterS("Wed, 21 Oct 2026 07:28:00 GMT"), (uint32_t)0);
    CHECK_EQ(parseRetryAfterS("12abc"), (uint32_t)0);
    // Huge numbers do not overflow; the wait caps them anyway.
    CHECK(parseRetryAfterS("99999999999999999999") > RETRY_AFTER_MAX_S);
    CHECK_EQ(throttleWaitMs(0, parseRetryAfterS("99999999999999999999")), (uint32_t)3600000);
}

int main() {
    RUN(test_backoff_doubles_to_cap);
    RUN(test_retry_after_wins_only_when_longer);
    RUN(test_parse_retry_after);
    return SUMMARY();
}
