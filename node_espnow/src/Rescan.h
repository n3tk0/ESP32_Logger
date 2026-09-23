// ============================================================================
// node_espnow/src/Rescan.h
//
// What the node does when its reports stop being answered: whether it may look
// for the network yet, and what to do with what the scan found. Pure, so
// tests/host/test_espnow_node_cfgfetch.cpp can check every branch.
//
// docs/ESPNOW_NODE.md §2 has the reasoning for the gate (a rate limit counted
// in wakes, not a schedule) and docs/NODE_CONFIG.md §4.6 the handover rule
// (`link.next_ssid` is tried when the stored network is not on the air).
// ============================================================================
#pragma once

#include <stdint.h>

namespace enrescan {

/// May this wake scan? `failStreak` unanswered wakes in a row, `wakesSinceScan`
/// since the last scan (0xFFFF = never), against the config's
/// link.rescan_fails and link.rescan_min_s at `intervalS` a wake.
static inline bool due(uint8_t failStreak, uint16_t wakesSinceScan, uint8_t rescanFails,
                       uint32_t rescanMinS, uint16_t intervalS) {
    const uint32_t iv = intervalS ? intervalS : 60;
    const uint32_t wakesPerCeiling = (rescanMinS + iv - 1) / iv;
    const uint8_t  fails = rescanFails ? rescanFails : 1;
    return failStreak >= fails && wakesSinceScan >= wakesPerCeiling;
}

enum class Action : uint8_t {
    None,         ///< nothing to do (never returned today; kept for callers)
    MoveChannel,  ///< the stored network is on another channel: use it
    AdoptNext,    ///< the stored network is gone; next_ssid is on the air
    Sweep,        ///< run the signed DISCOVER sweep for the collector
};

/// Decide from one scan. `storedCh` / `nextCh` are the channels the stored
/// network (BSSID, then SSID) and link.next_ssid were heard on, 0 = not heard;
/// `currentCh` is the channel the node has been failing on.
///
/// * The stored network moved channel → follow it (the original case: a
///   router that re-picked its channel).
/// * The stored network is not on the air and next_ssid is → the collector's
///   handover happened while we slept (§4): promote next.
/// * The stored network is on the air on the channel we already use and next
///   is heard elsewhere → the old network stayed up and the collector moved;
///   promote next too. (Contract §4.6 says "when the stored SSID/BSSID is not
///   on the air"; an old network that stays on after a handover is the same
///   situation from the node's side, and waiting for it to vanish could be
///   forever.)
/// * Otherwise → sweep. That covers the network being gone entirely, and the
///   stored network being on our channel with nobody answering — a collector
///   that was reflashed (it can no longer decrypt us, so it can never say so)
///   or that moved to a network whose name we were never told. The collector
///   answers a signed DISCOVER from a node already in its table even with no
///   pairing window open (§4.6), so the sweep is how either comes back.
static inline Action decide(uint8_t storedCh, uint8_t nextCh, uint8_t currentCh) {
    if (storedCh && storedCh != currentCh) return Action::MoveChannel;
    if (nextCh && (!storedCh || nextCh != currentCh)) return Action::AdoptNext;
    return Action::Sweep;
}

}  // namespace enrescan
