// Host unit tests for src/core/SensorTypes.h
//   - SensorReading::toJsonLine() formatting, escaping and truncation
//   - parseMode() / parseBucket() string -> enum mapping
#include "src/core/SensorTypes.h"
#include "check.h"

static void test_toJsonLine_basic() {
    SensorReading r = SensorReading::make(1700000000u, "env1", "bme280",
                                          "temp", 21.5f, "C", QUALITY_GOOD);
    char buf[256];
    int n = r.toJsonLine(buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_STREQ(buf,
        "{\"ts\":1700000000,\"id\":\"env1\",\"sensor\":\"bme280\","
        "\"metric\":\"temp\",\"value\":21.5,\"unit\":\"C\",\"q\":1}");
    // Return value must equal the written length (snprintf semantics).
    CHECK_EQ((size_t)n, std::strlen(buf));
}

static void test_toJsonLine_escaping() {
    // id contains a double-quote, metric contains a backslash — both must be
    // JSON-escaped so the line stays parseable.
    SensorReading r = SensorReading::make(0u, "a\"b", "t", "x\\y",
                                          0.0f, "", QUALITY_UNKNOWN);
    char buf[256];
    int n = r.toJsonLine(buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(std::strstr(buf, "\"id\":\"a\\\"b\"")     != nullptr);  // a"b -> a\"b
    CHECK(std::strstr(buf, "\"metric\":\"x\\\\y\"") != nullptr);  // x\y -> x\\y
}

static void test_toJsonLine_control_char() {
    char id[4] = { 'a', '\n', 'b', '\0' };   // embedded newline ->

    SensorReading r = SensorReading::make(0u, id, "t", "m",
                                          1.0f, "u", QUALITY_GOOD);
    char buf[256];
    int n = r.toJsonLine(buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(std::strstr(buf, "\\u000a") != nullptr);
    // Guard against a boundary truncation embedding a NUL mid-line: the line
    // must be fully formed through the trailing fields and closing brace.
    CHECK(std::strstr(buf, ",\"metric\":\"m\"") != nullptr);
    CHECK(buf[n - 1] == '}');
    CHECK_EQ((size_t)n, std::strlen(buf));   // no embedded NUL
}

static void test_toJsonLine_truncation() {
    SensorReading r = SensorReading::make(1700000000u, "env1", "bme280",
                                          "temp", 21.5f, "C", QUALITY_GOOD);
    char small[8];
    int n = r.toJsonLine(small, sizeof(small));
    CHECK_EQ(n, -1);   // signals truncation, never a bogus positive length
}

// Sweep every buffer size against a reading whose id holds a control char
// (forces a \uXXXX escape, the boundary case that motivated the a+6 guard in
// _appendJsonEscaped).  Invariant for ALL sizes: either toJsonLine reports
// truncation (-1), or it returns a length with NO embedded NUL (strlen == n)
// and a well-formed line ('{' ... '}').
static void test_toJsonLine_buffer_sweep() {
    char id[4] = { 'a', '\n', 'b', '\0' };
    SensorReading r = SensorReading::make(123u, id, "bme280", "temp",
                                          1.0f, "C", QUALITY_GOOD);
    char buf[200];
    bool ok = true;
    for (size_t cap = 1; cap <= sizeof(buf); cap++) {
        int n = r.toJsonLine(buf, cap);
        if (n < 0) continue;                       // truncation reported — fine
        if ((size_t)n != strlen(buf)) ok = false;  // embedded NUL on "success"
        if ((size_t)n >= cap)         ok = false;   // must fit when not -1
        if (buf[0] != '{' || buf[n-1] != '}') ok = false;
    }
    CHECK(ok);
}

static void test_parseMode() {
    CHECK_EQ(parseMode("raw"),  AGG_RAW);
    CHECK_EQ(parseMode("avg"),  AGG_AVG);
    CHECK_EQ(parseMode("min"),  AGG_MIN);
    CHECK_EQ(parseMode("max"),  AGG_MAX);
    CHECK_EQ(parseMode("lttb"), AGG_LTTB);
    CHECK_EQ(parseMode("sum"),  AGG_SUM);
    CHECK_EQ(parseMode("nonsense"), AGG_LTTB);  // default
    CHECK_EQ(parseMode(nullptr),    AGG_LTTB);  // null-safe default
}

static void test_parseBucket() {
    CHECK_EQ(parseBucket("raw"), BUCKET_RAW);
    CHECK_EQ(parseBucket("1m"),  BUCKET_1MIN);
    CHECK_EQ(parseBucket("5m"),  BUCKET_5MIN);
    CHECK_EQ(parseBucket("1h"),  BUCKET_1HOUR);
    CHECK_EQ(parseBucket("1d"),  BUCKET_1DAY);
    CHECK_EQ(parseBucket("zzz"), BUCKET_5MIN);  // default
    CHECK_EQ(parseBucket(nullptr), BUCKET_5MIN);// null-safe default
}

// ---------------------------------------------------------------------------
// readingIsBackfilled()
// ---------------------------------------------------------------------------
// The predicate decides whether a reading reaches the live dashboard and the
// alert engine at all, and every way it can be wrong is silent — there is no
// log line for "excluded as history". So each branch is pinned here.
static void test_backfill_window() {
    const uint32_t now = 1800000000u;   // a real epoch, mid-2027

    CHECK(!readingIsBackfilled(now,        now));   // this instant
    CHECK(!readingIsBackfilled(now - 1u,   now));
    CHECK(!readingIsBackfilled(now - 120u, now));   // the window is inclusive
    CHECK( readingIsBackfilled(now - 121u, now));   // and one second past it
    CHECK( readingIsBackfilled(now - 86400u, now)); // a day of buffered outage

    // The window is a parameter, and the boundary holds wherever it is put.
    CHECK(!readingIsBackfilled(now - 300u, now, 300u));
    CHECK( readingIsBackfilled(now - 301u, now, 300u));
}

// THE REGRESSION THIS FILE EXISTS FOR.
//
// Both stamps are uint32_t. Before the guard, a reading one second ahead of
// the comparison clock wrapped the subtraction to ~4.29 billion and was
// classed as backfill — which silently emptied the live web ring and stopped
// alerts firing, on a device whose only symptom was a dashboard that had
// stopped updating.
//
// It was not a corner case: SensorTask stamps from the hardware RTC when one
// is fitted, ProcessingTask compares against the system clock, and two clocks
// that agree to the second are the exception rather than the rule.
static void test_a_stamp_from_the_future_is_not_backfill() {
    const uint32_t now = 1800000000u;

    CHECK(!readingIsBackfilled(now + 1u,     now));   // the RTC one second ahead
    CHECK(!readingIsBackfilled(now + 60u,    now));   // a minute ahead
    CHECK(!readingIsBackfilled(now + 86400u, now));   // a badly wrong clock

    // Stated as the property rather than as three examples: nothing in the
    // future is ever history, however far ahead it claims to be.
    for (uint32_t ahead = 1; ahead < 4000000000u; ahead = ahead * 3u + 1u)
        CHECK(!readingIsBackfilled(now + ahead, now));
}

// No usable clock on either side means nothing can be judged, and an
// unjudgeable reading takes the live path rather than vanishing from it.
static void test_an_unset_clock_never_hides_a_reading() {
    const uint32_t now = 1800000000u;

    CHECK(!readingIsBackfilled(0u, now));            // the reading has no stamp
    CHECK(!readingIsBackfilled(now - 86400u, 0u));   // the device has no clock
    CHECK(!readingIsBackfilled(0u, 0u));

    // millis()-derived fallback stamps are small numbers, not epochs; they
    // must not be read as 1970 and buried as fifty years of history.
    CHECK(!readingIsBackfilled(1u, now));
    CHECK(!readingIsBackfilled(999999999u, now));    // one short of the floor
}

int main() {
    RUN(test_backfill_window);
    RUN(test_a_stamp_from_the_future_is_not_backfill);
    RUN(test_an_unset_clock_never_hides_a_reading);
    RUN(test_toJsonLine_basic);
    RUN(test_toJsonLine_escaping);
    RUN(test_toJsonLine_control_char);
    RUN(test_toJsonLine_truncation);
    RUN(test_toJsonLine_buffer_sweep);
    RUN(test_parseMode);
    RUN(test_parseBucket);
    return SUMMARY();
}
