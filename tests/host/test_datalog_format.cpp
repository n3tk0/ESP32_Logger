// Host unit tests for src/storage/DatalogFormat.cpp
//   - the original data_log row, byte for byte, for each date / time / end /
//     volume format and the optional fields
//   - the header line and the field positions readers take from a layout
//   - sensor columns: values, empty fields, and the empty tail cut off
//   - header detection
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/storage/DatalogFormat.cpp"
#include "check.h"

// 2026-09-29 07:31:12 UTC; TZ is UTC below, so local == UTC.
static const uint32_t T0 = 1790667072;

static DatalogLayout legacyLayout() {
    DatalogLayout l = {};
    l.dateFormat = 1; l.timeFormat = 0; l.endFormat = 1; l.volumeFormat = 0;
    l.boot = true; l.volume = true; l.ff = true; l.pf = true;
    return l;
}

static DatalogRow btnRow() {
    DatalogRow r = {};
    r.start = T0; r.end = T0 + 45; r.boot = 123;
    strcpy(r.trigger, "FF_BTN");
    r.volume = 1.25f; r.ff = 0; r.pf = 1;
    return r;
}

// ---------------------------------------------------------------------------
static void test_original_row() {
    char b[256];
    DatalogLayout l = legacyLayout();
    CHECK(dlFormatRow(b, sizeof(b), l, btnRow(), nullptr, 0) > 0);
    CHECK_STREQ(b, "29/09/2026|07:31:12|45s|#:123|FF_BTN|L:1,25|FF0|PF1");

    l.dateFormat = 3; l.timeFormat = 1; l.endFormat = 0; l.volumeFormat = 1;
    dlFormatRow(b, sizeof(b), l, btnRow(), nullptr, 0);
    CHECK_STREQ(b, "2026-09-29|07:31|07:31|#:123|FF_BTN|L:1.25|FF0|PF1");

    l.dateFormat = 0; l.timeFormat = 2; l.boot = false; l.volumeFormat = 2;
    l.ff = l.pf = false;
    dlFormatRow(b, sizeof(b), l, btnRow(), nullptr, 0);
    CHECK_STREQ(b, "7:31:12AM|7:31:57AM|FF_BTN|1.25");

    // No end time known: the zeroed clock the original printed.
    l = legacyLayout(); l.endFormat = 0;
    DatalogRow r = btnRow(); r.end = 0;
    dlFormatRow(b, sizeof(b), l, r, nullptr, 0);
    CHECK_STREQ(b, "29/09/2026|07:31:12|00:00:00|#:123|FF_BTN|L:1,25|FF0|PF1");

    l = legacyLayout(); l.dateFormat = 2;
    dlFormatRow(b, sizeof(b), l, btnRow(), nullptr, 0);
    CHECK(strncmp(b, "09/29/2026|", 11) == 0);
    l.dateFormat = 4;
    dlFormatRow(b, sizeof(b), l, btnRow(), nullptr, 0);
    CHECK(strncmp(b, "29.09.2026|", 11) == 0);
}

// ---------------------------------------------------------------------------
static void test_header_and_index() {
    char b[256];
    const char* labels[] = { "env_temperature", "a|b" };
    DatalogLayout l = legacyLayout();
    CHECK(dlFormatHeader(b, sizeof(b), l, labels, 2) > 0);
    CHECK_STREQ(b, "Date|Start|Duration|Boot|Trigger|Volume|FF|PF|env_temperature|a_b");
    DatalogFieldIdx ix = dlFieldIndex(l);
    CHECK_EQ(ix.date, 0); CHECK_EQ(ix.trigger, 4); CHECK_EQ(ix.pf, 7); CHECK_EQ(ix.nBase, 8);

    l.dateFormat = 0; l.endFormat = 2; l.boot = false; l.volume = false; l.ff = false; l.pf = false;
    dlFormatHeader(b, sizeof(b), l, labels, 1);
    CHECK_STREQ(b, "Start|Trigger|env_temperature");
    ix = dlFieldIndex(l);
    CHECK_EQ(ix.date, -1); CHECK_EQ(ix.start, 0); CHECK_EQ(ix.end, -1);
    CHECK_EQ(ix.trigger, 1); CHECK_EQ(ix.volume, -1); CHECK_EQ(ix.nBase, 2);

    l.endFormat = 0;
    dlFormatHeader(b, sizeof(b), l, nullptr, 0);
    CHECK_STREQ(b, "Start|End|Trigger");

    // Too small: refused, not truncated.
    CHECK_EQ(dlFormatHeader(b, 10, legacyLayout(), labels, 2), -1);
}

// ---------------------------------------------------------------------------
static void test_sensor_columns() {
    char b[256];
    DatalogLayout l = legacyLayout();
    l.endFormat = 0;
    DatalogRow r = {};
    r.start = T0; r.end = T0 + 60; r.boot = 7;
    strcpy(r.trigger, DL_TRIGGER_TIMER);
    r.volume = NAN; r.ff = -1; r.pf = -1;
    float v[] = { 21.4f, NAN, 1013.25f, 55.0f };
    dlFormatRow(b, sizeof(b), l, r, v, 4);
    CHECK_STREQ(b, "29/09/2026|07:31:12|07:32:12|#:7|TIMER||||21.4||1013.25|55");

    // An empty tail is cut: the row ends at its last value.
    float w[] = { 21.4f, NAN, NAN };
    dlFormatRow(b, sizeof(b), l, r, w, 3);
    CHECK_STREQ(b, "29/09/2026|07:31:12|07:32:12|#:7|TIMER||||21.4");
    float none[] = { NAN, NAN };
    dlFormatRow(b, sizeof(b), l, r, none, 2);
    CHECK_STREQ(b, "29/09/2026|07:31:12|07:32:12|#:7|TIMER");

    // A button row with no sensor values reads like the original format.
    float empty[] = { NAN, NAN };
    dlFormatRow(b, sizeof(b), legacyLayout(), btnRow(), empty, 2);
    CHECK_STREQ(b, "29/09/2026|07:31:12|45s|#:123|FF_BTN|L:1,25|FF0|PF1");

    // Too small: refused, not truncated.
    CHECK_EQ(dlFormatRow(b, 20, l, r, v, 4), -1);
}

static void test_values() {
    char b[24];
    dlFormatValue(b, sizeof(b), 21.0f);     CHECK_STREQ(b, "21");
    dlFormatValue(b, sizeof(b), 21.456f);   CHECK_STREQ(b, "21.46");
    dlFormatValue(b, sizeof(b), -0.001f);   CHECK_STREQ(b, "0");
    dlFormatValue(b, sizeof(b), -3.5f);     CHECK_STREQ(b, "-3.5");
    dlFormatValue(b, sizeof(b), 1234.5f);   CHECK_STREQ(b, "1234.5");
    CHECK_EQ(dlFormatValue(b, sizeof(b), NAN), 0);
    CHECK_STREQ(b, "");
}

static void test_header_detection() {
    CHECK(dlIsHeaderLine("Date|Start|Trigger"));
    CHECK(dlIsHeaderLine("Start|Trigger|env_2_temperature"));
    CHECK(!dlIsHeaderLine("29/09/2026|07:31:12|FF_BTN"));
    CHECK(!dlIsHeaderLine("7:31:12AM|FF_BTN"));
    CHECK(!dlIsHeaderLine(""));
}

int main() {
    setenv("TZ", "UTC0", 1);
    tzset();
    RUN(test_original_row);
    RUN(test_header_and_index);
    RUN(test_sensor_columns);
    RUN(test_values);
    RUN(test_header_detection);
    return SUMMARY();
}
