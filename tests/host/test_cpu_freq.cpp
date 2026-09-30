// Host unit tests for src/utils/CpuFreq.h — which CPU speeds the settings
// accept on a C3 (160 MHz max) and an S3 (240 MHz max), and the default the
// web-active speed falls back to.
#include "src/utils/CpuFreq.h"
#include "check.h"

static void test_valid_values() {
    CHECK(cpuMhzValid(80, 160));
    CHECK(cpuMhzValid(160, 160));
    CHECK(!cpuMhzValid(240, 160));               // a C3 cannot run it
    CHECK(cpuMhzValid(240, 240));                // an S3 can
    CHECK(!cpuMhzValid(0, 240));
    CHECK(!cpuMhzValid(40, 240));                // below 80 Wi-Fi stops
    CHECK(!cpuMhzValid(120, 240));
}

static void test_active_default() {
    // 0 is what a config from before the byte reads: the old fixed 160.
    CHECK_EQ((int)cpuActiveMhzFor(0, 160), 160);
    CHECK_EQ((int)cpuActiveMhzFor(0, 240), 160);
}

static void test_stored_value_wins() {
    CHECK_EQ((int)cpuActiveMhzFor(80, 160), 80);
    CHECK_EQ((int)cpuActiveMhzFor(240, 240), 240);
    // 240 carried from an S3 config onto a C3 falls back, never reaches
    // setCpuFrequencyMhz().
    CHECK_EQ((int)cpuActiveMhzFor(240, 160), 160);
    CHECK_EQ((int)cpuActiveMhzFor(99, 240), 160);
}

int main() {
    RUN(test_valid_values);
    RUN(test_active_default);
    RUN(test_stored_value_wins);
    return SUMMARY();
}
