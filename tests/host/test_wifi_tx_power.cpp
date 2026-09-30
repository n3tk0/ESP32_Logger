// Host unit tests for src/utils/WifiTxPower.h — the Wi-Fi transmit power
// setting and the board default it falls back to.
#include "src/utils/WifiTxPower.h"
#include "check.h"

static void test_valid_values() {
    CHECK(wifiTxPowerValid(0));                  // board default
    for (uint8_t v : WIFI_TX_POWERS) CHECK(wifiTxPowerValid(v));
    CHECK(!wifiTxPowerValid(1));
    CHECK(!wifiTxPowerValid(80));                // above the radio's maximum
    CHECK(!wifiTxPowerValid(-4));
    CHECK(!wifiTxPowerValid(255));
}

static void test_board_default() {
    CHECK_EQ((int)wifiTxPowerFor(0, true),  34); // C3 Pico / Super Mini: 8.5 dBm
    CHECK_EQ((int)wifiTxPowerFor(0, false), 78); // everything else: full power
}

static void test_stored_value_wins() {
    CHECK_EQ((int)wifiTxPowerFor(78, true),  78); // full power asked for on a Pico
    CHECK_EQ((int)wifiTxPowerFor(52, false), 52);
    // A byte this firmware does not offer is the board default, not garbage
    // handed to the radio.
    CHECK_EQ((int)wifiTxPowerFor(99, true),  34);
    CHECK_EQ((int)wifiTxPowerFor(99, false), 78);
}

int main() {
    RUN(test_valid_values);
    RUN(test_board_default);
    RUN(test_stored_value_wins);
    return SUMMARY();
}
