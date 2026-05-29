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

int main() {
    RUN(test_toJsonLine_basic);
    RUN(test_toJsonLine_escaping);
    RUN(test_toJsonLine_control_char);
    RUN(test_toJsonLine_truncation);
    RUN(test_parseMode);
    RUN(test_parseBucket);
    return SUMMARY();
}
