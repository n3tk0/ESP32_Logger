// ============================================================================
// src/espnow/EspNowIngest.h
//
// The collector's ESP-NOW radio: receive frames from battery nodes, answer
// them, and hand what arrives to the same mailbox the HTTP ingest path uses.
//
// WHERE A READING GOES
// --------------------
// Straight into RemoteIngest, exactly as POST /api/ingest does. Everything
// downstream is then free: RemoteNodeSensor drains the mailbox on the normal
// sensor tick, so an ESP-NOW reading gets the same calibration, the same
// outlier filters, the same ring buffer, the same exporters and the same
// dashboard as a wired BME280. Nothing in the pipeline knows or needs to know
// that this one arrived over a radio.
//
// That is why this file is small. The work was choosing to land in an
// existing mailbox instead of building a second path.
//
// TWO THINGS THAT MUST BE TRUE
// ----------------------------
// 1. Modem sleep OFF. WIFI_PS_MIN_MODEM breaks ESP-NOW unicast — measured —
//    while leaving broadcast working, which is the worst possible failure
//    mode: pairing succeeds and then no reading ever arrives. The sketch's
//    modemSleepAllowed() returns false whenever this feature is compiled in —
//    asked at the point of use, not applied to the config once, because a
//    device with no sleep settings at all never reaches the parser that would
//    have applied it.
//
// 2. The channel is not ours to pick. ESP-NOW rides the STA interface and
//    uses whatever channel the access point put us on. Peers are therefore
//    added with channel 0, meaning "current", and the collector simply
//    follows its router. The node is the side that has to catch up — see
//    docs/ESPNOW_NODE.md for why that asymmetry is forced.
//
// NOT YET ON HARDWARE
// -------------------
// None of this has run on a board. The parts that could be tested without one
// are in NodeTable.h and BatteryModel.h with host tests; what is left here is
// radio API calls and the receive callback, and those are honest guesses at
// how the hardware behaves until somebody flashes it.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_ESPNOW_INGEST

#include <Arduino.h>

#include "NodeTable.h"

/// Bring up ESP-NOW on the STA interface.
///
/// Call AFTER WiFi has been started and ideally after it has associated: the
/// peer channel is inherited from the interface, so initialising before the
/// station knows its channel means adding peers on channel 0 of a radio that
/// has not settled yet.
///
/// Returns false if the radio refused to start, in which case nothing else
/// here does anything and the collector carries on without remote nodes.
bool espnowIngestBegin();

/// Move whatever the receive callback parked into RemoteIngest, and refresh
/// the derived battery metrics.
///
/// Call from loop() or any ordinary task. Never from the callback: this walks
/// the mailbox, may touch the filesystem, and the callback runs on the WiFi
/// task where neither belongs.
void espnowIngestTick();

/// How long the collector listens for a node when it has none provisioned,
/// and the default length of a window opened from the web interface.
///
/// In the header rather than the implementation because there are now two
/// callers that must agree on it: the boot-time window, and POST
/// /api/espnow/pair when it is asked to open one without a duration.
#ifndef ESPNOW_BOOT_PAIRING_S
#  define ESPNOW_BOOT_PAIRING_S 120
#endif

/// Open a pairing window for `seconds`. While it is open, a DISCOVER frame
/// carrying a valid signature gets a node slot and a WELCOME reply. Outside
/// it, DISCOVER is ignored — which is most of what stops a passing stranger's
/// node from being adopted.
void espnowBeginPairing(uint32_t seconds);
bool espnowPairingActive();

/// Provision a node directly, bypassing the pairing handshake. Returns false
/// when the table is full or `nodeId` is outside 1..254.
bool espnowAddNode(const uint8_t mac[6], uint8_t nodeId, const char* label,
                   uint16_t intervalS);

/// Forget a node: drops the radio peer and the slot, and persists the change.
bool espnowRemoveNode(uint8_t nodeId);

/// Copy the provisioned nodes out for a caller that wants to iterate them —
/// the status endpoint, the dashboard.
///
/// A copy and not a reference, because the table is written by the ingest
/// tick and read by whatever task is rendering, and handing out a pointer
/// into it would be handing out a data race. Returns how many entries were
/// written, up to `maxOut`.
int espnowCopyNodes(EspNowNode* out, int maxOut);

/// How far a node's clock is from the collector's, in seconds, positive when
/// the node is BEHIND us — which is the direction an RC-timed deep sleep
/// drifts. Returns false when there is no measurement yet: the node has never
/// reported, or one of the two clocks was unset when it did.
///
/// Measured rather than reported. Every DATA frame already carries the node's
/// own epoch and this collector has a real clock, so the difference costs the
/// node nothing — no extra byte on the air, no extra wake, and no flash write
/// on a device that spends its life asleep.
///
/// Deliberately NOT stored in EspNowNode: the table is persisted as raw
/// structs, so a new field there changes sizeof(), makes loadNodes() discard
/// the saved file, and costs every deployed node a re-pair. A live measurement
/// that is retaken on the next report should not be paid for at that price, so
/// it lives in a parallel array and starts empty after a reboot.
bool espnowNodeSkew(uint8_t nodeId, int32_t& outSkewS);

/// True when any tracked node's battery warrants the dashboard warning.
bool espnowAnyBatteryWarn();

/// Nodes that have missed ESPNOW_OFFLINE_INTERVALS reports in a row.
int espnowOfflineCount();

/// Frames dropped since boot, by reason — for diagnostics, because "no
/// readings arrived" has several very different causes and they are
/// indistinguishable without these.
struct EspNowIngestStats {
    uint32_t framesRx;        ///< accepted DATA frames
    uint32_t malformed;       ///< failed espnowValidate()
    uint32_t unknownNode;     ///< valid frame, no such node provisioned
    uint32_t replayed;        ///< duplicate or stale sequence number
    uint32_t ringFull;        ///< arrived faster than the tick drained
    /// Backfilled readings dropped because the history queue was already
    /// full and had to shed its oldest to take a newer one. A backlog that
    /// keeps overflowing means the drain is too slow for the burst size —
    /// a tuning fact, and one worth being able to see rather than guess at.
    uint32_t historyCollapsed;
    /// Backfilled readings that had no date to be filed under: neither the
    /// node nor the collector had a real clock when the burst arrived.
    ///
    /// Counted apart from historyCollapsed, which it used to be folded into,
    /// because the two ask for opposite things. An overflowing queue wants a
    /// bigger queue; a burst with no clock wants NTP or an RTC, and no amount
    /// of queue will help it. One number covering both is the kind of
    /// diagnostic that sends people to tune the thing that was never wrong.
    uint32_t historyNoClock;
    uint32_t acksSent;
    uint32_t discoverSeen;
    uint32_t discoverBadSig;
    uint32_t paired;
};
const EspNowIngestStats& espnowStats();

/// Where the offline threshold is persisted. Named here so the writer and the
/// reader cannot drift apart — they were two string literals and a doc comment
/// that named a third spelling.
#define ESPNOW_NVS_NS          "espnow"
#define ESPNOW_NVS_OFFLINE_IV  "offline_iv"

/// Accepted range for the runtime threshold, outside of which
/// espnowSetOfflineIntervals() refuses.
#define ESPNOW_OFFLINE_INTERVALS_MIN 2
#define ESPNOW_OFFLINE_INTERVALS_MAX 60

/// Missed reports before a node counts as offline, as configured at runtime.
///
/// In NVS rather than in DeviceConfig: this is a diagnostic threshold, not part
/// of the device's identity, and putting it in config.bin would have grown the
/// struct and cost a migration on every deployed device for one byte.
///
/// Returns the compile-time ESPNOW_OFFLINE_INTERVALS when never set or stored
/// as 0, so "unset" and "the default" are the same thing to every caller.
uint8_t espnowGetOfflineIntervals();

/// Store a new threshold. 0 restores the built-in default; anything else must
/// be within [ESPNOW_OFFLINE_INTERVALS_MIN, ESPNOW_OFFLINE_INTERVALS_MAX].
/// Returns false — and changes nothing — when the value is out of range or the
/// write fails.
bool espnowSetOfflineIntervals(uint8_t n);

#endif  // FEATURE_ESPNOW_INGEST
