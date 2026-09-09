// ============================================================================
// node/src/Backlog.h — what the node keeps when it cannot reach the collector
//
// WHY THIS IS ITS OWN HEADER
// --------------------------
// The same reason NodePins.h is: two readers need the same answer and one of
// them is a desktop g++. Nothing in here touches WiFi, HTTP or a sensor — it
// is a ring buffer and some arithmetic — so it is the part of the node's
// store-and-forward that CAN be tested without a board, and it is the part
// where being wrong is quietest. A ring that drops the wrong end, or an age
// that goes backwards across the millis() wrap, produces a chart with a kink
// in it three weeks from now on a device nobody can attach a console to.
//
// WHAT IT HOLDS, AND WHY NOT SAMPLES
// ----------------------------------
// Readings, flat, each with its own age — not moments each holding an array of
// readings. Grouping by moment would size every slot for NODE_MAX_READINGS
// whether a node reports three metrics or twelve, and make the node that
// reports three pay for the twelve. The collector treats each reading
// independently anyway (each carries its own `dt_s`), so the grouping would
// buy nothing.
//
// metric and unit are stored as pointers because the sensor layer hands back
// static strings — see NodeReading in sensors.h. Nothing here outlives the
// program, so nothing here has to own them.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

namespace NodeBacklog {

/// 192 readings is an hour of a three-metric node at the default one-minute
/// interval, or twenty minutes of a nine-metric one, for about 3 KB of the
/// ESP8266's static RAM. Raise it if the node has room: the only cost is RAM,
/// and the only thing that shrinks the window it buys is a longer outage.
static const int CAPACITY = 192;

struct Entry {
    uint32_t    ms;       ///< millis() when it was measured
    float       value;
    const char* metric;   ///< static, owned by the sensor layer
    const char* unit;
};

/// How many whole seconds ago `thenMs` was.
///
/// UNSIGNED SUBTRACTION, DELIBERATELY. millis() wraps every 49.7 days, and on
/// a node that is meant to run for months that is not a corner case, it is a
/// date in the calendar. `now - then` in unsigned arithmetic is the true
/// elapsed time straight through the wrap; comparing the two values instead —
/// `now > then ? now - then : 0` — reads as caution and is the version that
/// breaks, turning every queued reading into a fresh one for the 49 days after
/// each wrap.
inline uint32_t ageSeconds(uint32_t nowMs, uint32_t thenMs) {
    return (uint32_t)(nowMs - thenMs) / 1000u;
}

class Ring {
public:
    /// Append one reading, dropping the OLDEST when full.
    ///
    /// Losing the start of an outage beats losing the end of it: the recent
    /// hours are the ones the dashboard draws and the ones somebody is looking
    /// at. It is the same rule RemoteIngest::putHistorical applies at the
    /// collector, so a reading does not survive one queue to be dropped by the
    /// other's opposite opinion.
    void push(const Entry& e) {
        if (_count == CAPACITY) {
            _head = (_head + 1) % CAPACITY;
            _count--;
            _dropped++;
        }
        _buf[(_head + _count) % CAPACITY] = e;
        _count++;
    }

    /// Forget the oldest `n` — called only once the collector has said what it
    /// did with them.
    void drop(int n) {
        if (n <= 0) return;
        if (n > _count) n = _count;
        _head   = (_head + n) % CAPACITY;
        _count -= n;
    }

    /// Oldest first: at(0) is the reading that has been waiting longest.
    const Entry& at(int i) const { return _buf[(_head + i) % CAPACITY]; }

    int  count() const { return _count; }
    bool full()  const { return _count == CAPACITY; }

    /// How many readings have been discarded to make room, ever. Reported so
    /// an outage longer than the buffer is a number in the log rather than a
    /// silence — the node cannot keep them, but it can say how many it lost.
    uint32_t dropped() const { return _dropped; }

private:
    Entry    _buf[CAPACITY] = {};
    int      _head    = 0;
    int      _count   = 0;
    uint32_t _dropped = 0;
};

}  // namespace NodeBacklog
