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
    /// Buffered samples whose environmental values the mailbox could not keep.
    /// RemoteIngest holds one value per (node, metric), so a burst's older
    /// readings are overwritten by its newer ones. Counted rather than hidden:
    /// a node spending airtime on readings dropped at this end should be
    /// visible somewhere.
    uint32_t historyCollapsed;
    uint32_t acksSent;
    uint32_t discoverSeen;
    uint32_t discoverBadSig;
    uint32_t paired;
};
const EspNowIngestStats& espnowStats();

#endif  // FEATURE_ESPNOW_INGEST
