// ============================================================================
// node_espnow/src/Link.h
//
// The node's radio: bring ESP-NOW up, send one frame, wait briefly for the
// answer, and — when the answers stop coming — go and find where the network
// moved to.
//
// Everything here is written for a part that is awake for about a third of a
// second and then gone. There are no retries beyond what is described, no
// background tasks, and no state that outlives the wake except what main.cpp
// deliberately puts in RTC memory.
// ============================================================================
#pragma once

#include <stdint.h>

#include "src/espnow/EspNowProto.h"

/// What the node knows about its collector. Persisted in NVS by main.cpp.
struct NodeLink {
    uint8_t  nodeId;        ///< 0 = never provisioned
    uint8_t  channel;       ///< 0 = unknown
    uint8_t  collector[6];  ///< the collector's STA MAC
    uint8_t  bssid[6];      ///< the access point, for finding a moved channel
    char     ssid[33];
    uint16_t intervalS;
};

/// The outcome of one report. Every field is something the caller acts on.
struct LinkResult {
    bool     sent;        ///< the radio accepted and delivered it at MAC level
    bool     acked;       ///< the collector's ACK arrived inside the window
    uint32_t waitedMs;    ///< how long the receive window actually cost
    uint32_t epoch;       ///< wall clock from the ACK, 0 if none
    uint16_t intervalS;   ///< requested wake interval, 0 = keep the current one
    uint8_t  channel;     ///< the channel the collector reported being on
    bool     rediscover;  ///< the collector asked for a fresh pairing
};

/// Start the radio, optionally pinned to `channel` (0 = leave it alone).
/// Adds the broadcast peer, and the collector as an encrypted peer when
/// `link.nodeId` says we have one.
bool linkBegin(const NodeLink& link);

/// Stop the radio cleanly before sleeping.
void linkEnd();

/// Send one DATA frame and wait up to NODE_ACK_WINDOW_MS for the reply.
///
/// The wait ENDS THE MOMENT THE REPLY ARRIVES — the window is a ceiling, not a
/// duration, and `LinkResult::waitedMs` reports what it actually cost so the
/// battery arithmetic can be checked against reality rather than assumed.
LinkResult linkSend(const NodeLink& link, const DataMsg& msg, uint8_t count);

/// Sweep the channels broadcasting a signed DISCOVER until a WELCOME for this
/// node comes back. On success `io` holds everything the collector sent.
///
/// Costs up to NODE_MAX_CHANNEL × NODE_PAIR_DWELL_MS of radio — about 1.6 s
/// for thirteen channels — so the caller rate-limits it.
bool linkPair(NodeLink& io);

/// Passive scan for the stored access point. Returns its channel, or 0 when it
/// is not on the air.
///
/// Matches on BSSID first because that is exact, then falls back to SSID: a
/// mesh or a repeater changes the BSSID under you while the SSID stays put.
uint8_t linkFindChannel(const NodeLink& link);

/// This node's STA MAC, for the DISCOVER frame and for matching a WELCOME.
const uint8_t* linkOwnMac();
