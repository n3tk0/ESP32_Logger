// ============================================================================
// node_espnow/src/node_config.h
//
// Everything about this node that a person might want to change, in one place.
// Each is overridable from platformio.ini with -D, so a second node with a
// different sensor address or a different divider does not need this file
// edited.
// ============================================================================
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Keys — must match the collector
// ---------------------------------------------------------------------------
// One shared 16-byte secret. It encrypts the link (as the ESP-NOW LMK) and it
// authorises this node to be adopted during a pairing window (as the HMAC key
// on the DISCOVER frame). If it does not match the collector's, nothing pairs
// and nothing decrypts, and neither side will say anything more useful than
// "bad signature".
#ifndef ESPNOW_LMK
#  define ESPNOW_LMK "change-this-key!"
#endif
#ifndef ESPNOW_PMK
#  define ESPNOW_PMK "esp32-logger-pmk"
#endif
static_assert(sizeof(ESPNOW_LMK) == 17, "ESPNOW_LMK must be exactly 16 characters");
static_assert(sizeof(ESPNOW_PMK) == 17, "ESPNOW_PMK must be exactly 16 characters");

// ---------------------------------------------------------------------------
// Cadence
// ---------------------------------------------------------------------------

/// Seconds between wakes. The collector can change this at runtime through the
/// ACK; this is only the value used before it has ever answered.
///
/// 60 s costs roughly 10 mAh/day and 30 s roughly 19 — see docs/ESPNOW_NODE.md
/// for where those come from and why the real figure is shorter than the
/// arithmetic suggests.
#ifndef NODE_INTERVAL_S
#  define NODE_INTERVAL_S 60
#endif

/// Longest the node holds its radio in receive waiting for the collector's
/// ACK. A CEILING, not a duration: the wait ends the moment the frame arrives,
/// which is normally a few milliseconds.
///
/// The distinction is the whole battery argument. A fixed 30 ms at one wake a
/// minute would be about 1 mAh/day — a tenth of the budget — spent almost
/// entirely on waiting after the answer had already come.
#ifndef NODE_ACK_WINDOW_MS
#  define NODE_ACK_WINDOW_MS 30
#endif

/// Consecutive wakes with no ACK before the node goes looking for a moved
/// channel. Three, because a single lost frame is ordinary in a shared band.
#ifndef NODE_RESCAN_FAILS
#  define NODE_RESCAN_FAILS 3
#endif

/// Least time between two channel scans.
///
/// A RATE LIMIT, NOT A SCHEDULE, and the difference matters in both
/// directions. A collector that is simply switched off would otherwise make
/// this node spend 1.5–2 s of radio every single minute — an order of
/// magnitude more than a normal wake — so the ceiling turns a dead collector
/// from a battery emergency into a rounding error. But a channel move at 14:03
/// is recovered at the next wake and not at 15:00, because the hour is
/// measured from the last scan rather than from a clock. Losing an hour of
/// readings to a router reboot would be the wrong trade.
#ifndef NODE_RESCAN_MIN_INTERVAL_S
#  define NODE_RESCAN_MIN_INTERVAL_S 3600
#endif

/// Milliseconds spent listening on each channel during the pairing sweep.
/// Long enough for the collector to answer, short enough that a full sweep of
/// thirteen channels is under two seconds.
#ifndef NODE_PAIR_DWELL_MS
#  define NODE_PAIR_DWELL_MS 120
#endif

/// Highest channel the pairing sweep tries. 13 covers ETSI; 11 is the FCC
/// limit and sweeping past it costs a fifth of a second per attempt for
/// nothing in that regulatory domain.
#ifndef NODE_MAX_CHANNEL
#  define NODE_MAX_CHANNEL 13
#endif

// ---------------------------------------------------------------------------
// Sensor — BME280 or BMP280 on I2C
// ---------------------------------------------------------------------------
// XIAO ESP32-C3 defaults: D4 = GPIO6 = SDA, D5 = GPIO7 = SCL.
#ifndef NODE_I2C_SDA
#  define NODE_I2C_SDA 6
#endif
#ifndef NODE_I2C_SCL
#  define NODE_I2C_SCL 7
#endif

/// 0x76 with SDO to ground, 0x77 with SDO to VCC. Most breakout boards tie it
/// low; a few do not, and the node tries the other address if the first is
/// silent rather than making that a build-time decision somebody has to
/// discover.
#ifndef NODE_BMX_ADDR
#  define NODE_BMX_ADDR 0x76
#endif

// ---------------------------------------------------------------------------
// Battery sensing
// ---------------------------------------------------------------------------

/// ADC pin the divider's midpoint goes to. A0 on the XIAO ESP32-C3.
#ifndef NODE_BATT_PIN
#  define NODE_BATT_PIN 2
#endif

/// Ratio of cell voltage to what the pin sees. 2.0 for two equal resistors.
///
/// TWO 220 kΩ RESISTORS, PERMANENTLY CONNECTED. That is about 9.5 µA at 4.2 V,
/// under a percent of the daily budget at one-minute intervals — cheaper than
/// the MOSFET and the GPIO it would take to switch the divider off, and one
/// less thing to fail closed. Larger resistors would draw less and would also
/// stop the ADC's input from settling within a sample.
///
/// A 4.2 V cell puts 2.1 V on the pin, comfortably inside the roughly 2.5 V
/// the C3's ADC reaches at 11 dB.
#ifndef NODE_BATT_DIVIDER
#  define NODE_BATT_DIVIDER 2.0f
#endif

/// Per-node trim, applied after the divider ratio. 1.0 disables it.
///
/// The resistors are 1 % at best and the ADC reference varies part to part, so
/// two nodes built identically will disagree by tens of millivolts. Measure
/// the cell with a meter, divide by what the node reports, and put the result
/// here — it is the difference between a remaining-life estimate that means
/// something and one that is confidently wrong.
#ifndef NODE_BATT_TRIM
#  define NODE_BATT_TRIM 1.0f
#endif

/// ADC samples averaged per reading. Sixteen costs microseconds and takes most
/// of the noise out of a measurement the whole battery estimate rests on.
#ifndef NODE_BATT_SAMPLES
#  define NODE_BATT_SAMPLES 16
#endif

// ---------------------------------------------------------------------------
// Bench build
// ---------------------------------------------------------------------------
// -DNODE_NO_DEEP_SLEEP keeps the part awake between sends, so the serial
// console stays attached and a pairing attempt or a channel rescan can be
// watched happening. Useless on a battery, and the firmware says so at boot
// rather than leaving somebody to wonder why the cell lasted two days.
