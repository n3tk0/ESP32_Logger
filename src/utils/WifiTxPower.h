// ============================================================================
// src/utils/WifiTxPower.h — the Wi-Fi transmit power setting
// (config.network.txPower), without Arduino, so the host tests can check it.
//
// The value is ESP-IDF's own unit, a quarter of a dBm (wifi_power_t: 78 is
// 19.5 dBm, 34 is 8.5 dBm). 0 means "the board's default".
//
// WHY THE BOARD DECIDES THE DEFAULT: the LOLIN C3 Pico and the C3 Super Mini
// brown out at the full 19.5 dBm. Their regulator and antenna cannot carry the
// current a burst of transmitting draws, and loading the web page is such a
// burst (a device on 2026-09-30 logged BROWNOUT and stayed dead until it was
// unplugged). 8.5 dBm is the usual cure for both. Every other board keeps
// the radio's full power, as it always had.
// ============================================================================
#pragma once

#include <stdint.h>

// The powers the settings offer, strongest first (all are wifi_power_t values).
static const uint8_t WIFI_TX_POWERS[] = { 78, 68, 60, 52, 44, 34, 28, 20 };
constexpr uint8_t WIFI_TX_FULL = 78;   // 19.5 dBm, the radio's default
constexpr uint8_t WIFI_TX_LOW  = 34;   //  8.5 dBm

// 0 (the board's default) or one of WIFI_TX_POWERS.
inline bool wifiTxPowerValid(int q) {
    if (q == 0) return true;
    for (uint8_t v : WIFI_TX_POWERS) if (v == q) return true;
    return false;
}

// The power to set: the stored one, or for 0 (and anything invalid) the
// board's default. `lowPowerBoard` = a LOLIN C3 Pico or a C3 Super Mini.
inline uint8_t wifiTxPowerFor(uint8_t stored, bool lowPowerBoard) {
    if (stored != 0 && wifiTxPowerValid(stored)) return stored;
    return lowPowerBoard ? WIFI_TX_LOW : WIFI_TX_FULL;
}
