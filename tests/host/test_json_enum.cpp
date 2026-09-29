// Host unit tests for src/utils/JsonEnum.h — reading a module's enum field.
//
// The Settings › Modules form used to post a <select>'s value as a string,
// and `cfg["rotation"] | (int)cur` keeps `cur` for "2", so saving a new
// theme mode or WiFi mode answered ok and changed nothing.
#include "src/utils/JsonEnum.h"
#include "check.h"

static int read(const char* json, int def) {
    JsonDocument d;
    deserializeJson(d, json);
    return jsonEnumInt(d["v"], def);
}

static void test_numbers_and_numeric_strings() {
    CHECK(read("{\"v\":2}", 7) == 2);
    CHECK(read("{\"v\":0}", 7) == 0);
    CHECK(read("{\"v\":\"2\"}", 7) == 2);        // what the form used to send
    CHECK(read("{\"v\":\"0\"}", 7) == 0);
    CHECK(read("{\"v\":\"-1\"}", 7) == -1);      // range is the caller's clamp
}

static void test_anything_else_keeps_default() {
    CHECK(read("{}", 7) == 7);                    // absent
    CHECK(read("{\"v\":null}", 7) == 7);
    CHECK(read("{\"v\":\"\"}", 7) == 7);
    CHECK(read("{\"v\":\"dark\"}", 7) == 7);
    CHECK(read("{\"v\":\"2x\"}", 7) == 7);        // not a whole number
    CHECK(read("{\"v\":true}", 7) == 7);
}

int main() {
    RUN(test_numbers_and_numeric_strings);
    RUN(test_anything_else_keeps_default);
    return SUMMARY();
}
