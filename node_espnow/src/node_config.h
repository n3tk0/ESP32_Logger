// ============================================================================
// node_espnow/src/node_config.h
//
// Everything about this node that a person might want to change, in one place.
// Each is overridable from platformio.ini with -D, so a second node with a
// different sensor address or a different divider does not need this file
// edited.
//
// SINCE THE CONFIG DOCUMENT (docs/NODE_CONFIG.md): most values here are now
// only DEFAULTS. The node's settings live in NVS as the §1 document and are
// edited on its own page or from the collector; these values seed that
// document on a node that has none (ConfigStore.cpp, cfgStoreDefaults()) and
// are otherwise not read. Marked "default" below. The keys, the radio
// constants and the portal constants are still used as they are.
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

/// Default interval_s. Seconds between wakes. The collector can change this at
/// runtime (the config document, or the ACK's legacy interval field); this is
/// only the value used before anything has.
///
/// 60 s costs roughly 10 mAh/day and 30 s roughly 19 — see docs/ESPNOW_NODE.md
/// for where those come from and why the real figure is shorter than the
/// arithmetic suggests.
#ifndef NODE_INTERVAL_S
#  define NODE_INTERVAL_S 60
#endif

/// Default link.ack_window_ms. Longest the node holds its radio in receive
/// waiting for the collector's ACK. A CEILING, not a duration: the wait ends the moment the frame arrives,
/// which is normally a few milliseconds.
///
/// The distinction is the whole battery argument. A fixed 30 ms at one wake a
/// minute would be about 1 mAh/day — a tenth of the budget — spent almost
/// entirely on waiting after the answer had already come.
#ifndef NODE_ACK_WINDOW_MS
#  define NODE_ACK_WINDOW_MS 30
#endif

/// Default link.rescan_fails. Consecutive wakes with no ACK before the node
/// goes looking for a moved channel. Three, because a single lost frame is ordinary in a shared band.
#ifndef NODE_RESCAN_FAILS
#  define NODE_RESCAN_FAILS 3
#endif

/// Default link.rescan_min_s. Least time between two channel scans.
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
// Sensor — defaults for the one BME280 or BMP280 a fresh node starts with
// ---------------------------------------------------------------------------
// The sensor list is the config document's `sensors`; these only seed it.
// XIAO ESP32-C3 defaults: D4 = GPIO6 = SDA, D5 = GPIO7 = SCL.
#ifndef NODE_I2C_SDA
#  define NODE_I2C_SDA 6
#endif
#ifndef NODE_I2C_SCL
#  define NODE_I2C_SCL 7
#endif

/// 0x76 with SDO to ground, 0x77 with SDO to VCC. 0 (the default) = probe
/// 0x76 then 0x77: most breakout boards tie SDO low, a few do not, and trying
/// the other address costs a millisecond rather than making it a build-time
/// decision somebody has to discover. That was this node's behaviour before
/// the config document, and "addr": 0 is how the document says it.
#ifndef NODE_BMX_ADDR
#  define NODE_BMX_ADDR 0
#endif

// ---------------------------------------------------------------------------
// Battery sensing
// ---------------------------------------------------------------------------

/// Default batt.pin. ADC pin the divider's midpoint goes to. A0 on the XIAO
/// ESP32-C3.
#ifndef NODE_BATT_PIN
#  define NODE_BATT_PIN 2
#endif

/// Default batt.divider. Ratio of cell voltage to what the pin sees. 2.0 for
/// two equal resistors.
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

/// Default batt.trim. Per-node trim, applied after the divider ratio. 1.0
/// disables it.
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
// Config exchange with the collector (docs/NODE_CONFIG.md §5, CfgFetch.h)
// ---------------------------------------------------------------------------

/// Whole-fetch ceiling, in ms, for pulling a changed config within one wake.
///
/// Six 200-byte slices is the largest document there can be; answered from
/// the collector's receive callback each is a few ms, so a real fetch is tens
/// of ms. The ceiling is for a collector that flags a config and then does
/// not answer: 400 ms of receive at ~85 mA is ~0.009 mAh, and the backoff in
/// CfgFetch.h stops that from repeating every wake. Past the ceiling the
/// wake gives up and the next one starts again from offset 0.
#ifndef NODE_CFG_BUDGET_MS
#  define NODE_CFG_BUDGET_MS 400
#endif

/// Per-request reply ceiling for CFG_GET and for the last CFG_REPORT slice.
/// The larger of this and link.ack_window_ms is used: a slice is a bigger
/// frame than an ACK and the collector may do a little more work for it.
#ifndef NODE_CFG_REPLY_MS
#  define NODE_CFG_REPLY_MS 50
#endif

/// Unanswered CFG_GETs in a row before the wake gives up.
#ifndef NODE_CFG_MAX_MISSES
#  define NODE_CFG_MAX_MISSES 2
#endif

// ---------------------------------------------------------------------------
// Firmware updates from the collector (docs/NODE_OTA.md §4, FwFetch.h)
// ---------------------------------------------------------------------------

/// Most time one wake spends downloading an image, in ms, on a battery.
///
/// A ~1 MB image is ~6500 slices and ~320 sector erases: 40–60 s of radio in
/// receive at ~85 mA, about 1–1.5 mAh — once per update, a few hours of the
/// node's ordinary budget. Split over wakes so no single wake holds the node
/// (and its readings) up for a minute, and so a collector that stops
/// answering costs one budget, not a flat cell. 20 s is three or four wakes
/// for a whole image.
#ifndef NODE_FW_BUDGET_MS
#  define NODE_FW_BUDGET_MS 20000
#endif

/// The same in mains mode, where the energy is free and the only cost is the
/// report that waits: two minutes normally covers the whole image in one go.
#ifndef NODE_FW_BUDGET_MAINS_MS
#  define NODE_FW_BUDGET_MAINS_MS 120000
#endif

/// Per-request reply ceiling for FW_GET. The larger of this and
/// link.ack_window_ms is used. The collector answers from its receive
/// callback when the slice is in its RAM window, and not at all while it
/// loads the next one from the card — so this is sized for the answer, and
/// the next setting for the refill.
#ifndef NODE_FW_REPLY_MS
#  define NODE_FW_REPLY_MS 50
#endif

/// Unanswered FW_GETs in a row before the wake gives up. A window refill
/// from the SD card costs a request or two; six in a row is a collector that
/// is gone, or busy with something else.
#ifndef NODE_FW_MAX_MISSES
#  define NODE_FW_MAX_MISSES 6
#endif

// ---------------------------------------------------------------------------
// The setup page (docs/NODE_CONFIG.md §6, Portal.h)
// ---------------------------------------------------------------------------

/// The button that opens the page. GPIO9 = BOOT on the XIAO ESP32-C3.
#ifndef NODE_PORTAL_PIN
#  define NODE_PORTAL_PIN 9
#endif

/// After a power-on or a press of RESET, how long the node watches BOOT
/// before carrying on. BOOT held THROUGH reset selects the C3's ROM download
/// mode, so the gesture is "RESET, then hold BOOT" and the firmware has to be
/// looking when it happens. Paid only on those boots, never on a wake.
#ifndef NODE_PORTAL_WINDOW_MS
#  define NODE_PORTAL_WINDOW_MS 2000
#endif

/// The setup AP's WPA2 password — at least 8 characters. The same default as
/// the WiFi node's; change it.
#ifndef PORTAL_AP_PASS
#  define PORTAL_AP_PASS "configure"
#endif
static_assert(sizeof(PORTAL_AP_PASS) >= 9, "PORTAL_AP_PASS: WPA2 needs 8+ characters");

/// Idle time before the page gives up and restarts the node. The clock only
/// runs while nobody is connected to the AP.
#ifndef PORTAL_TIMEOUT_MS
#  define PORTAL_TIMEOUT_MS 300000UL   // 5 minutes
#endif

/// Reported as `fw` in the config document, and the version in the image's
/// NODEFW1 marker (docs/NODE_OTA.md §1): [A-Za-z0-9._+-], at most 23 chars.
#ifndef NODE_FW_VERSION
#  define NODE_FW_VERSION "2026.09.1"
#endif

// ---------------------------------------------------------------------------
// Bench build
// ---------------------------------------------------------------------------
// -DNODE_NO_DEEP_SLEEP keeps the part awake between sends, so the serial
// console stays attached and a pairing attempt or a channel rescan can be
// watched happening. Useless on a battery, and the firmware says so at boot
// rather than leaving somebody to wonder why the cell lasted two days.
