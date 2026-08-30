// ============================================================================
// src/espnow/NodeTable.h
//
// What the collector remembers about each battery node, and the three
// decisions it makes about an arriving frame:
//
//   1. is this sequence number one we should accept?
//   2. which metrics does this sample actually contain?
//   3. has this node gone quiet?
//
// All of it is plain data and free functions with no Arduino in sight, which
// is deliberate: EspNowIngest.cpp is then thin glue over logic that
// tests/host/test_espnow_nodetable.cpp can exercise on the build host. The
// parts of this feature that can be tested without a radio are the parts most
// likely to be wrong, and none of them should be buried in a receive callback.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include "EspNowProto.h"
#include "../power/BatteryModel.h"

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

/// Nodes the collector will track. ESP-NOW itself allows six encrypted peers
/// on this IDF, so eight slots is already more than the radio will carry —
/// the extra two exist so a replaced node can be added before the old entry
/// is removed.
#ifndef ESPNOW_MAX_NODES
#  define ESPNOW_MAX_NODES 8
#endif

/// Assumed reporting period when a node has not told us its own.
#ifndef ESPNOW_DEFAULT_INTERVAL_S
#  define ESPNOW_DEFAULT_INTERVAL_S 60
#endif

/// Missed reports before a node counts as offline.
///
/// Three, not one: a single lost frame is ordinary — 2.4 GHz is a shared band
/// and ESP-NOW has no retransmission above the MAC layer — and marking a node
/// offline for one miss would make the dashboard flicker. Three consecutive
/// misses is no longer bad luck.
#ifndef ESPNOW_OFFLINE_INTERVALS
#  define ESPNOW_OFFLINE_INTERVALS 3
#endif

// ---------------------------------------------------------------------------
// The metrics a node reports
// ---------------------------------------------------------------------------
// Names and units match what a wired BME280 produces, because they end up in
// the same pipeline through the same RemoteIngest mailbox: a remote reading
// should be indistinguishable from a local one downstream, and it is not if
// the outdoor node calls its pressure "press" or reports it in pascals while
// every other sensor reports hectopascals.

#define EN_METRIC_TEMPERATURE "temperature"
#define EN_METRIC_HUMIDITY    "humidity"
#define EN_METRIC_PRESSURE    "pressure"
#define EN_METRIC_BATT_V      "battery_voltage"
#define EN_METRIC_BATT_PCT    "battery_percent"
#define EN_METRIC_BATT_DAYS   "battery_days"

// SensorReading::metric is char[16] and SensorReading::make() strncpy's into
// it, so a 17-character name is stored truncated with no error anywhere and
// every later strcmp() against the full name misses. That is precisely how
// "humidity_ambient" shipped broken.
//
// tools/check_metric_names.py catches this for the sensor plugins by parsing
// their make()/getMetrics() calls, but these names are #defines and it would
// not see them. So they are checked here instead, where the compiler does it.
// This is also why the metric is battery_days and not battery_days_left:
// the latter is 17 characters and would have been silently cut to
// "battery_days_lef".
static_assert(sizeof(EN_METRIC_TEMPERATURE) <= 16, "metric would be truncated");
static_assert(sizeof(EN_METRIC_HUMIDITY)    <= 16, "metric would be truncated");
static_assert(sizeof(EN_METRIC_PRESSURE)    <= 16, "metric would be truncated");
static_assert(sizeof(EN_METRIC_BATT_V)      <= 16, "metric would be truncated");
static_assert(sizeof(EN_METRIC_BATT_PCT)    <= 16, "metric would be truncated");
static_assert(sizeof(EN_METRIC_BATT_DAYS)   <= 16, "metric would be truncated");

/// One metric ready to be handed to RemoteIngest::put().
struct EspNowMetric {
    const char* metric;
    float       value;
    const char* unit;
};

/// Most metrics one sample can produce: temperature, humidity, pressure,
/// battery voltage.
static const int EN_MAX_SAMPLE_METRICS = 4;

/// Most metrics one node can produce in a tick: the four above, plus the two
/// the collector derives from the battery history.
static const int EN_MAX_NODE_METRICS = 6;

/// Expand one wire sample into the metrics it actually carries.
///
/// An absent field produces NO metric — not a zero. That is the whole point
/// of the sentinels in EspNowProto.h: a BMP280 has no humidity sensor, and a
/// `0 %RH` in the pipeline would be indistinguishable from a real reading and
/// would drag every average and every chart down with it.
///
/// Pressure is converted from the pascals on the wire to the hectopascals
/// every other sensor in this firmware reports, and battery millivolts to
/// volts, for the same reason: downstream must not be able to tell a remote
/// reading from a wired one.
static inline int espnowExpandSample(const EnvSample& s, EspNowMetric* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    int n = 0;

    const float t = enUnpackTemp(s.t_c100);
    if (!enIsAbsent(t) && n < maxOut) out[n++] = {EN_METRIC_TEMPERATURE, t, "C"};

    const float rh = enUnpackRh(s.rh_x100);
    if (!enIsAbsent(rh) && n < maxOut) out[n++] = {EN_METRIC_HUMIDITY, rh, "%"};

    const float pa = enUnpackPress(s.press_pa);
    if (!enIsAbsent(pa) && n < maxOut) out[n++] = {EN_METRIC_PRESSURE, pa / 100.0f, "hPa"};

    const float mv = enUnpackMv(s.vbat_mv);
    if (!enIsAbsent(mv) && n < maxOut) out[n++] = {EN_METRIC_BATT_V, mv / 1000.0f, "V"};

    return n;
}

// ---------------------------------------------------------------------------
// Sequence numbers
// ---------------------------------------------------------------------------

enum EspNowSeqVerdict : uint8_t {
    EN_SEQ_NEW,        ///< accept and store
    EN_SEQ_RESET,      ///< the node restarted; accept and store
    EN_SEQ_DUPLICATE,  ///< already seen this exact frame; drop
    EN_SEQ_STALE,      ///< older than what we hold; drop
};

/// Decide what to do with an arriving sequence number.
///
/// The comparison is modular, because `seq` is a 16-bit counter that wraps
/// after 65,536 frames — about six weeks at one a minute. Treating 0 as
/// "older than 65535" would make a node go silent for six weeks at the wrap,
/// so newer-ness is the sign of the 16-bit difference instead: anything
/// within half the range ahead is newer.
///
/// ORDER MATTERS HERE. The duplicate test runs BEFORE the first-boot escape,
/// and that ordering is the only thing limiting replay. A node that resets
/// starts counting from zero again, so EN_FLAG_FIRST_BOOT has to be honoured
/// or a rebooted node would be ignored until its counter caught up — but that
/// makes a captured first-boot frame replayable. Rejecting the exact sequence
/// number we last accepted means such a frame is accepted at most once, and
/// then not again until the real node moves the counter on.
///
/// This is a mitigation and not a fix. The encryption underneath (CCMP, per
/// peer LMK) is what actually protects these frames; this is the guard for
/// the ordinary case, which is the radio's own retry delivering a duplicate.
static inline EspNowSeqVerdict espnowSeqCheck(bool haveSeq, uint16_t last,
                                              uint16_t seq, uint8_t flags) {
    if (!haveSeq) return EN_SEQ_NEW;

    const uint16_t delta = (uint16_t)(seq - last);
    if (delta == 0)      return EN_SEQ_DUPLICATE;
    if (delta < 0x8000u) return EN_SEQ_NEW;
    if (flags & EN_FLAG_FIRST_BOOT) return EN_SEQ_RESET;
    return EN_SEQ_STALE;
}

// ---------------------------------------------------------------------------
// Clock skew
// ---------------------------------------------------------------------------

/// Seconds a node's clock is out, positive when the node is BEHIND `ours` —
/// which is the direction an RC-timed deep sleep drifts. Returns false, and
/// leaves `out` alone, when either clock is unset.
///
/// Both guards are load-bearing and neither is redundant. A node that has never
/// been told the time sends epoch 0, and "1970 minus now" is not a drift
/// measurement, it is the absence of one. The collector's own clock is equally
/// capable of being unset, and subtracting from a zero there would make every
/// node look fifty-six years fast.
///
/// The subtraction widens to int64 first. The obvious `(int32_t)(ours - theirs)`
/// on two uint32s is right for small differences by accident of two's
/// complement and wrong for large ones, and the case where it is wrong — a node
/// whose clock is years out because it was never synchronised — is exactly the
/// case this function exists to report.
///
/// Anything beyond ±2^31 seconds saturates rather than wrapping: a skew of
/// sixty-eight years is not a number anyone reads, but it must not come back as
/// a small one.
///
/// The negative clamp stops one short of INT32_MIN, deliberately. Callers take
/// the magnitude with `-skew`, and negating INT32_MIN is undefined behaviour —
/// a range this function can never produce is cheaper than a range every caller
/// has to remember to handle.
static inline bool espnowClockSkew(uint32_t ours, uint32_t theirs, int32_t& out) {
    if (ours < 1000000000u || theirs < 1000000000u) return false;

    const int64_t d = (int64_t)ours - (int64_t)theirs;
    if (d >  2147483647LL)       out =  2147483647L;
    else if (d < -2147483647LL)  out = -2147483647L;
    else                         out = (int32_t)d;
    return true;
}

// ---------------------------------------------------------------------------
// A node
// ---------------------------------------------------------------------------

struct EspNowNode {
    bool     used;
    uint8_t  mac[6];
    uint8_t  nodeId;             ///< 1..254, the id carried on the wire
    char     id[17];             ///< what lands in SensorReading::sensorId
    uint16_t intervalS;          ///< expected seconds between reports

    // Link state
    bool     everSeen;
    bool     haveSeq;
    uint16_t lastSeq;
    uint32_t lastSeenMs;         ///< millis() at the last accepted frame
    uint32_t framesRx;
    uint32_t framesDropped;      ///< duplicates and stale sequence numbers
    int8_t   rssi;

    // Battery
    uint16_t       lastMv;       ///< 0 when the node has never reported one
    BatteryHistory batt;
};

/// Default label for a node with no user-supplied name: "espnow-07".
///
/// Nine characters into SensorReading::sensorId's seventeen, which leaves
/// room for a user to rename it to something meaningful without the rename
/// silently truncating.
static inline void espnowDefaultNodeId(uint8_t nodeId, char* out, size_t outLen) {
    if (!out || outLen < 10) { if (out && outLen) out[0] = '\0'; return; }
    out[0] = 'e'; out[1] = 's'; out[2] = 'p'; out[3] = 'n';
    out[4] = 'o'; out[5] = 'w'; out[6] = '-';
    out[7] = (char)('0' + (nodeId / 10) % 10);
    out[8] = (char)('0' + nodeId % 10);
    out[9] = '\0';
}

/// Has this node missed enough reports to be called offline?
///
/// A node that has never reported is offline, which is what makes a
/// provisioned-but-never-heard-from node visible instead of merely absent.
///
/// The age is an unsigned difference so it stays correct across the millis()
/// wrap at ~49 days — a collector that has been up longer than that must not
/// suddenly declare every node offline.
static inline bool espnowNodeOffline(const EspNowNode& n, uint32_t nowMs,
                                     uint8_t intervals = ESPNOW_OFFLINE_INTERVALS) {
    if (!n.used || !n.everSeen) return true;
    const uint32_t iv = n.intervalS ? n.intervalS : (uint32_t)ESPNOW_DEFAULT_INTERVAL_S;
    const uint32_t limitMs = iv * 1000u * (uint32_t)intervals;
    return (uint32_t)(nowMs - n.lastSeenMs) > limitMs;
}

/// The two figures the collector derives rather than receives.
///
/// Emitted only when they mean something: no battery voltage has ever been
/// reported means neither is emitted, and an undeterminable remaining life
/// (see batteryDaysLeft()) omits `battery_days` rather than publishing a
/// placeholder. A metric that is absent from the pipeline shows as a gap; a
/// metric published as -1 shows as a reading of minus one day.
static inline int espnowBatteryMetrics(const EspNowNode& n, EspNowMetric* out, int maxOut) {
    if (!out || maxOut <= 0 || n.lastMv == 0) return 0;
    int i = 0;

    // No bounds test on the first: maxOut > 0 is established above, so the
    // slot exists. The second one needs it, because the first may have taken
    // the only slot there was.
    out[i++] = {EN_METRIC_BATT_PCT, (float)batteryPercent(n.lastMv), "%"};

    const int16_t days = batteryDaysLeft(n.batt, n.lastMv);
    if (days >= 0 && i < maxOut) out[i++] = {EN_METRIC_BATT_DAYS, (float)days, "d"};

    return i;
}

/// Should the dashboard warn about this node's battery?
static inline bool espnowNodeBatteryWarn(const EspNowNode& n) {
    if (!n.used || n.lastMv == 0) return false;
    return batteryShouldWarn(batteryPercent(n.lastMv),
                             batteryDaysLeft(n.batt, n.lastMv));
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

/// Fixed-size, no allocation, linear scan. Eight entries is not a data
/// structure problem.
///
/// Slots are claimed on provisioning and released only by an explicit
/// remove(), never by silence — the same rule RemoteIngest follows, and for
/// the same reason: a node that stops reporting should go visibly offline,
/// not quietly surrender its slot to whatever turns up next.
class EspNowNodeTable {
public:
    static constexpr int CAP = ESPNOW_MAX_NODES;

    EspNowNodeTable() { clear(); }

    /// Value-initialisation rather than memset: EspNowNode holds a
    /// BatteryHistory, which has a constructor, so memset over it is the
    /// kind of thing -Wclass-memaccess exists to point at.
    void clear() { for (int i = 0; i < CAP; i++) _n[i] = EspNowNode{}; }

    EspNowNode* byId(uint8_t nodeId) {
        if (nodeId == 0) return nullptr;
        for (int i = 0; i < CAP; i++)
            if (_n[i].used && _n[i].nodeId == nodeId) return &_n[i];
        return nullptr;
    }

    /// Slot index for `nodeId`, or -1. For state kept ALONGSIDE the table
    /// rather than inside it — see the clock-skew array in EspNowIngest.cpp.
    /// Adding a field to EspNowNode changes sizeof and makes loadNodes()
    /// discard the saved file, which costs every deployed node a re-pair; a
    /// live measurement that is re-taken on the next report has no business
    /// paying that.
    int indexOf(uint8_t nodeId) const {
        if (nodeId == 0) return -1;
        for (int i = 0; i < CAP; i++)
            if (_n[i].used && _n[i].nodeId == nodeId) return i;
        return -1;
    }

    EspNowNode* byMac(const uint8_t* mac) {
        if (!mac) return nullptr;
        for (int i = 0; i < CAP; i++)
            if (_n[i].used && memcmp(_n[i].mac, mac, 6) == 0) return &_n[i];
        return nullptr;
    }

    /// Claim a slot. Returns the existing entry if `nodeId` is already known,
    /// so provisioning the same node twice updates it rather than consuming a
    /// second slot. Returns nullptr when full or when nodeId is out of range.
    EspNowNode* add(const uint8_t* mac, uint8_t nodeId, const char* label,
                    uint16_t intervalS) {
        if (nodeId == 0 || nodeId == 255 || !mac) return nullptr;

        EspNowNode* n = byId(nodeId);
        if (!n) {
            for (int i = 0; i < CAP; i++) {
                if (!_n[i].used) { n = &_n[i]; break; }
            }
            if (!n) return nullptr;
            *n = EspNowNode{};
            n->used   = true;
            n->nodeId = nodeId;
        }
        memcpy(n->mac, mac, 6);
        n->intervalS = intervalS ? intervalS : (uint16_t)ESPNOW_DEFAULT_INTERVAL_S;
        if (label && *label) {
            strncpy(n->id, label, sizeof(n->id) - 1);
            n->id[sizeof(n->id) - 1] = '\0';
        } else if (n->id[0] == '\0') {
            espnowDefaultNodeId(nodeId, n->id, sizeof(n->id));
        }
        return n;
    }

    bool remove(uint8_t nodeId) {
        EspNowNode* n = byId(nodeId);
        if (!n) return false;
        *n = EspNowNode{};
        return true;
    }

    int count() const {
        int c = 0;
        for (int i = 0; i < CAP; i++) if (_n[i].used) c++;
        return c;
    }

    int offlineCount(uint32_t nowMs, uint8_t intervals = ESPNOW_OFFLINE_INTERVALS) const {
        int c = 0;
        for (int i = 0; i < CAP; i++)
            if (_n[i].used && espnowNodeOffline(_n[i], nowMs, intervals)) c++;
        return c;
    }

    /// True when any tracked node's battery warrants the dashboard warning.
    bool anyBatteryWarn() const {
        for (int i = 0; i < CAP; i++)
            if (_n[i].used && espnowNodeBatteryWarn(_n[i])) return true;
        return false;
    }

    /// Slot access for callers that iterate — the web status endpoint and the
    /// tick that drains into RemoteIngest. Includes unused slots; check
    /// `.used`.
    EspNowNode&       at(int i)       { return _n[i]; }
    const EspNowNode& at(int i) const { return _n[i]; }

private:
    EspNowNode _n[CAP];
};
