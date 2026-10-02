// Host tests for heartbeatSilentMs() (src/tasks/Heartbeat.h), the software
// watchdog's silence measure.
#include "src/tasks/Heartbeat.h"
#include "check.h"

static void test_plain_gap() {
    CHECK_EQ(heartbeatSilentMs(100000, 70000), (uint32_t)30000);
    CHECK_EQ(heartbeatSilentMs(100000, 100000), (uint32_t)0);
}

// The bug: a task stamped millis() after the watchdog read `now`. Unsigned
// subtraction made that 4294967 s; it is a fresh heartbeat.
static void test_heartbeat_ahead_of_now_is_fresh() {
    CHECK_EQ(heartbeatSilentMs(100000, 100003), (uint32_t)0);
    CHECK_EQ(heartbeatSilentMs(100000, 100500), (uint32_t)0);
}

// millis() wraps after ~49.7 days; a real gap across the wrap still counts.
static void test_across_millis_wrap() {
    CHECK_EQ(heartbeatSilentMs(5000, 0xFFFFFFFFu - 24999u), (uint32_t)30000);
    CHECK_EQ(heartbeatSilentMs(0xFFFFFFF0u, 0xFFFFFFF0u + 4u), (uint32_t)0);
}

int main() {
    RUN(test_plain_gap);
    RUN(test_heartbeat_ahead_of_now_is_fresh);
    RUN(test_across_millis_wrap);
    return SUMMARY();
}
