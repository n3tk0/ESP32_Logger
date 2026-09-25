// ============================================================================
// node/src/NodeSync.h — the WiFi node's decisions about its config and its
// link, with nothing in them that needs a board.
//
// WHY THIS IS ITS OWN HEADER
// --------------------------
// For the reason Backlog.h and NodePins.h are: tests/host compiles it with a
// desktop g++. What lives here is every rule of docs/NODE_CONFIG.md §3 and §4
// that the node applies — which config in an ingest reply to take, which
// change needs a restart, which network to try this cycle, when to look for
// the collector on the LAN and when to give up on a config and roll it back.
// Each is a handful of comparisons, and each is the kind that is wrong
// quietly: a node that applies the same rejected config every minute, or
// rolls back a config that was fine, or never tries the network the
// collector moved to, looks connected on the bench and is not.
//
// main.cpp does the I/O (HTTP, LittleFS, WiFi.begin, UDP) and asks this
// header what to do. Nothing here allocates, prints or touches hardware.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include "src/nodecfg/NodeConfig.h"

namespace NodeSync {

using nodecfg::NodeConfig;
using nodecfg::SensorCfg;

// ---------------------------------------------------------------------------
// What changed between two configs (§3: "restart if network/sensor fields
// changed, else apply live")
// ---------------------------------------------------------------------------

enum ChangeFlags : uint8_t {
    CH_NONE    = 0,
    /// Something changed that takes effect without a restart: the name, the
    /// interval, the altitude, the board, net.next.
    CH_LIVE    = 1 << 0,
    /// The network settings (net.*, except `next`), the I2C pair or the sensor
    /// list changed. The node restarts: WiFi, the HTTP client's credentials,
    /// the background server's auth gate and every driver are brought up once,
    /// at boot, and a restart is the one path that is certainly right.
    CH_RESTART = 1 << 1,
    /// net.ssid, pass, host, port or token changed — the fields that decide
    /// whether this node can reach the collector at all. §4.7: the node runs
    /// the new config "on trial" and rolls it back after 5 cycles without a
    /// delivered POST.
    CH_NET_TRIAL = 1 << 2,
};

/// per_pulse has crossed JSON on its way here, often more than once: node →
/// /config.json → report → collector → reply. ArduinoJson writes a float
/// with six decimal places and does not always read that text back to the
/// same float, so an unchanged value can come back a few units off in its
/// last place — and == would call that a new sensor and restart the node on
/// a config that only renamed it. Measured over every float from 1e-9 to 1e9
/// (tests/host/test_node_sync.cpp): about 1% drift after the first trip, by
/// at most 8 ULP / 6.2e-7 relative over four. Two parts per million covers
/// that with room, and is far below what a pulse calibration means.
static inline bool samePerPulse(float a, float b) {
    if (a == b) return true;
    const float d  = (a > b) ? a - b : b - a;
    const float aa = (a < 0) ? -a : a;
    const float ab = (b < 0) ? -b : b;
    return d <= ((aa > ab) ? aa : ab) * 2e-6f;
}

static inline bool sameSensor(const SensorCfg& a, const SensorCfg& b) {
    return a.type == b.type && a.addr == b.addr && a.pin == b.pin && a.count == b.count &&
           strncmp(a.metric, b.metric, sizeof(a.metric)) == 0 && a.rx == b.rx &&
           a.tx == b.tx && a.mode == b.mode && samePerPulse(a.per_pulse, b.per_pulse) &&
           a.debounce_us == b.debounce_us;
}

static inline bool strDiff(const char* a, const char* b, size_t cap) {
    return strncmp(a, b, cap) != 0;
}

/// Which of the ChangeFlags going from `a` to `b` involves. rev/local and
/// the read-only identity fields are not settings and are not compared.
static inline uint8_t classifyChange(const NodeConfig& a, const NodeConfig& b) {
    uint8_t f = CH_NONE;

    const bool trial =
        strDiff(a.net.ssid, b.net.ssid, sizeof(a.net.ssid)) ||
        strDiff(a.net.pass, b.net.pass, sizeof(a.net.pass)) ||
        strDiff(a.net.host, b.net.host, sizeof(a.net.host)) ||
        a.net.port != b.net.port ||
        strDiff(a.net.token, b.net.token, sizeof(a.net.token));
    if (trial) f |= CH_NET_TRIAL | CH_RESTART;

    if (strDiff(a.net.basic_user, b.net.basic_user, sizeof(a.net.basic_user)) ||
        strDiff(a.net.basic_pass, b.net.basic_pass, sizeof(a.net.basic_pass)))
        f |= CH_RESTART;

    if (a.i2c.sda != b.i2c.sda || a.i2c.scl != b.i2c.scl) f |= CH_RESTART;

    if (a.sensor_count != b.sensor_count) {
        f |= CH_RESTART;
    } else {
        for (uint8_t i = 0; i < a.sensor_count && i < nodecfg::MAX_SENSORS; i++)
            if (!sameSensor(a.sensors[i], b.sensors[i])) { f |= CH_RESTART; break; }
    }

    if (strDiff(a.name, b.name, sizeof(a.name)) || a.interval_s != b.interval_s ||
        a.altitude_m != b.altitude_m || a.board != b.board ||
        strDiff(a.net.next.ssid, b.net.next.ssid, sizeof(a.net.next.ssid)) ||
        strDiff(a.net.next.pass, b.net.next.pass, sizeof(a.net.next.pass)))
        f |= CH_LIVE;

    return f;
}

// ---------------------------------------------------------------------------
// The ingest exchange (§3)
// ---------------------------------------------------------------------------

/// Does this POST carry `cfg` (the node's config, without secrets)? While
/// `local` (edited on the node's page, not yet adopted), and on the first
/// POST after boot — "first" meaning until one carrying it was answered, so a
/// boot whose first POST found no collector still reports on the next.
static inline bool shouldSendCfg(bool local, bool reportedSinceBoot) {
    return local || !reportedSinceBoot;
}

/// The `cfg` of an ingest reply, as far as the decision needs it.
struct ReplyCfg {
    bool     present = false;   ///< the reply had a "cfg" object
    bool     revOnly = false;   ///< it had nothing but "rev" — "your local config is now rev N"
    uint16_t rev     = 0;
};

enum class ReplyAction : uint8_t {
    None,      ///< nothing to do
    Confirm,   ///< the collector adopted the local config as `rev`: store rev, clear local
    Apply,     ///< a new config: decode, validate, back up, save, apply/restart
    Ignore,    ///< a config this node already refused (or a nonsense rev 0)
};

/// What to do with the reply's `cfg`, given the node's applied rev, its
/// `local` flag, and the rev it last refused (0 = none).
///
/// A refused rev is never re-validated: the collector is told once (cfg_error)
/// and stops sending it (§3: "no cfg_error for that rev"), but a collector that
/// had not yet seen the error — or an older one that never will — would
/// otherwise have the node re-decode and re-report the same bad config every
/// cycle.
static inline ReplyAction decideReply(const ReplyCfg& r, uint16_t haveRev, bool local,
                                      uint16_t rejectedRev) {
    if (!r.present) return ReplyAction::None;
    if (r.rev == 0) return ReplyAction::Ignore;
    if (r.revOnly) {
        if (local || r.rev != haveRev) return ReplyAction::Confirm;
        return ReplyAction::None;
    }
    if (rejectedRev != 0 && r.rev == rejectedRev) return ReplyAction::Ignore;
    if (r.rev != haveRev || local) return ReplyAction::Apply;
    return ReplyAction::None;
}

/// A refusal waiting to be reported as `cfg_error` — or, after a rollback
/// (§4.7), the rollback. Held until a POST carrying it is answered.
struct CfgError {
    uint16_t rev = 0;          ///< 0 = nothing to report
    char     field[24] = "";   ///< EN_CFG_FIELD_LEN: fits a CFG_ACK as is
    char     reason[48] = "";  ///< EN_CFG_REASON_LEN

    bool pending() const { return rev != 0; }
    void set(uint16_t r, const char* f, const char* why) {
        rev = r;
        nodecfg::copyStr(field, sizeof(field), f);
        nodecfg::copyStr(reason, sizeof(reason), why);
    }
    void clear() { rev = 0; field[0] = '\0'; reason[0] = '\0'; }
};

/// §4.7's words, used as the cfg_error reason after a rollback.
static const char ROLLBACK_FIELD[]  = "net";
static const char ROLLBACK_REASON[] = "rolled back: no collector on new settings";
static_assert(sizeof(ROLLBACK_REASON) <= sizeof(CfgError().reason), "fits cfg_error");

// ---------------------------------------------------------------------------
// The link: which network, when to look for the collector, when to roll back
// ---------------------------------------------------------------------------

enum class Net : uint8_t { Current, Next };

enum LinkAction : uint8_t {
    LA_NONE        = 0,
    /// Connected on net.next: make it the current network and keep the old
    /// one as next (§4.5), save with local = true.
    LA_PROMOTE     = 1 << 0,
    /// Broadcast the §3.1 query: right after a network switch, or after
    /// DISCOVER_AFTER_POST_FAILS failed POSTs in a row.
    LA_DISCOVER    = 1 << 1,
    /// The trial config reached a collector: it is kept (stop the trial).
    LA_TRIAL_OK    = 1 << 2,
    /// The trial config reached nothing for ROLLBACK_AFTER_CYCLES cycles:
    /// restore /config.prev.json, report cfg_error, restart (§4.7).
    LA_ROLLBACK    = 1 << 3,
};

class Link {
public:
    /// §4.5: "when the current network fails 2 cycles in a row and next is
    /// set, try next."
    static const uint8_t NEXT_AFTER_FAILS = 2;
    /// §3.1: "the configured host does not answer (3 consecutive failed POSTs)".
    static const uint8_t DISCOVER_AFTER_POST_FAILS = 3;
    /// §4.7: "if no POST succeeds in 5 cycles".
    static const uint8_t ROLLBACK_AFTER_CYCLES = 5;

    /// Start (or, after a restart, resume) running a collector-applied
    /// network change on trial.
    void beginTrial() { _trial = true; _trialCycles = 0; }
    void endTrial()   { _trial = false; _trialCycles = 0; }
    bool onTrial() const { return _trial; }

    /// Which network to try this cycle. The current one until it has failed
    /// NEXT_AFTER_FAILS cycles in a row; then, if there is a next, the two
    /// alternate — next first — for as long as neither connects.
    Net networkToTry(bool hasNext) const {
        if (!hasNext || _wifiFails < NEXT_AFTER_FAILS) return Net::Current;
        return ((_wifiFails - NEXT_AFTER_FAILS) % 2 == 0) ? Net::Next : Net::Current;
    }

    /// The outcome of this cycle's association attempt on `tried`.
    uint8_t wifiResult(bool connected, Net tried) {
        if (!connected) {
            // A u8 that stuck at 255 would freeze the alternation on one
            // network forever (after ~a day of failures at a 60 s interval).
            // Step back and forth between 254 and 255 instead: the count stays
            // far above NEXT_AFTER_FAILS and its parity keeps flipping.
            _wifiFails = (_wifiFails == 255) ? 254 : (uint8_t)(_wifiFails + 1);
            return LA_NONE;
        }
        _wifiFails = 0;
        if (tried == Net::Next) {
            // A new network is a new LAN: whatever address the collector had
            // on the old one means nothing here. Look before the first POST
            // rather than after three failures.
            _postFails = 0;
            return LA_PROMOTE | LA_DISCOVER;
        }
        return LA_NONE;
    }

    /// The outcome of one POST: `answered` is an HTTP 200 from /api/ingest,
    /// whatever it accepted — the collector is there.
    uint8_t postResult(bool answered) {
        if (answered) {
            _postFails = 0;
            _answeredThisCycle = true;
            if (_trial) { endTrial(); return LA_TRIAL_OK; }
            return LA_NONE;
        }
        if (++_postFails >= DISCOVER_AFTER_POST_FAILS) {
            // Counted afresh after each attempt, so a collector that is simply
            // off costs one broadcast every third cycle, not every cycle.
            _postFails = 0;
            return LA_DISCOVER;
        }
        return LA_NONE;
    }

    /// The end of a cycle (one interval): WiFi failed, POSTs failed, or at
    /// least one was answered.
    uint8_t cycleEnd() {
        const bool answered = _answeredThisCycle;
        _answeredThisCycle = false;
        if (!_trial || answered) return LA_NONE;
        if (++_trialCycles >= ROLLBACK_AFTER_CYCLES) {
            endTrial();
            return LA_ROLLBACK;
        }
        return LA_NONE;
    }

    uint8_t wifiFails() const { return _wifiFails; }
    uint8_t postFails() const { return _postFails; }
    uint8_t trialCycles() const { return _trialCycles; }

private:
    uint8_t _wifiFails   = 0;
    uint8_t _postFails   = 0;
    uint8_t _trialCycles = 0;
    bool    _trial       = false;
    bool    _answeredThisCycle = false;
};

/// §4.5: swap net.next into net, keeping the old network as next so a
/// cancelled switch is survivable. Marks the config local: the collector
/// learns of the swap from the next report.
static inline void promoteNext(NodeConfig& c) {
    nodecfg::NetNextCfg old;
    nodecfg::copyStr(old.ssid, sizeof(old.ssid), c.net.ssid);
    nodecfg::copyStr(old.pass, sizeof(old.pass), c.net.pass);
    nodecfg::copyStr(c.net.ssid, sizeof(c.net.ssid), c.net.next.ssid);
    nodecfg::copyStr(c.net.pass, sizeof(c.net.pass), c.net.next.pass);
    c.net.next = old;
    c.local = true;
}

// ---------------------------------------------------------------------------
// Metric names that outlive a reading
// ---------------------------------------------------------------------------

/// The backlog (Backlog.h) keeps a pointer to each reading's metric name, and
/// the sensor layer hands names back by value (NodeReading::name) because
/// they come from the config now — a ds18b20 bus's `metric` — not from string
/// literals. This is where each distinct name lives for the life of the
/// process, so a queued reading's pointer never dangles.
///
/// Sixteen is twice the metric budget: the names only change when the sensor
/// list does, which restarts the node, so a boot never sees more than eight.
class NameTable {
public:
    static const uint8_t CAP = 16;
    static const size_t  LEN = 16;   ///< nodecfg::METRIC_NAME_CAP

    /// The stored copy of `s`, adding it if new. nullptr when `s` is empty or
    /// the table is full — the caller drops that reading rather than store a
    /// pointer that would change under it.
    const char* intern(const char* s) {
        if (!s || !*s) return nullptr;
        for (uint8_t i = 0; i < _n; i++)
            if (strncmp(_names[i], s, LEN) == 0) return _names[i];
        if (_n >= CAP) return nullptr;
        nodecfg::copyStr(_names[_n], LEN, s);
        return _names[_n++];
    }
    uint8_t size() const { return _n; }

private:
    char    _names[CAP][LEN] = {};
    uint8_t _n = 0;
};

}  // namespace NodeSync
