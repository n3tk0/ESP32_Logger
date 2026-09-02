// ============================================================================
// src/sensors/RemoteIngest.h
//
// Mailbox for readings PUSHED to this device by remote sensor nodes.
//
// WHY A MAILBOX AND NOT A DIRECT PIPELINE WRITE
// ---------------------------------------------
// A node's HTTP POST lands on the AsyncTCP task. Writing straight into the
// pipeline from there would bypass everything SensorManager::tickFiltered()
// does on the way past — calibration axes, the outlier/rate filters, the
// ReadingCache feed that HeaterModule and BME688Sensor read, and the
// per-sensor enable flag. It would also put a pipeline producer on a task
// that must never block.
//
// So an ingest POST only drops values in this table. A RemoteNodeSensor
// plugin instance drains its own node's slots on the normal sensor tick, and
// from there the reading is indistinguishable from a locally wired one: same
// filters, same ring buffer, same exporters, same dashboard.
//
// The cost is latency — a value waits up to one read_interval_ms before it
// enters the pipeline. That is the right trade for a weather node reporting
// every 30-60 s, and it is why put() keeps the newest value per metric
// rather than queueing: a node that posts faster than the collector ticks
// should overwrite, not backlog.
//
// THREAD SAFETY
// -------------
// Writers: the AsyncTCP task, inside the /api/ingest handler.
// Readers: SensorTask, inside RemoteNodeSensor::readAll().
//
// A portMUX spinlock, for the same reasons as ReadingCache: critical
// sections are a bounded scan of short string compares, it is safe across
// both cores, and nothing else is acquired while it is held, so it cannot
// join a lock-ordering cycle. Notably it must NOT be configMutex — readAll()
// runs inside tickFiltered(), which already holds it.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "../core/SensorTypes.h"

class RemoteIngest {
public:
    // A node posts one metric set per interval. Four nodes × eight metrics
    // covers the intended shape (a handful of BME280/BMP280 satellites)
    // without making the linear scan interesting. Slots are claimed
    // first-come and never freed: a node that stops reporting goes stale
    // rather than surrendering its slot to a newcomer, so a flapping node
    // cannot evict a healthy one.
    static constexpr int MAX_ENTRIES        = 32;
    static constexpr int MAX_NODE_ID        = 17;   // matches SensorReading::sensorId
    static constexpr int MAX_METRIC         = 16;   // matches SensorReading::metric
    static constexpr int MAX_UNIT           = 12;   // matches SensorReading::unit

    /// Record one pushed metric for `nodeId`. Overwrites the previous value
    /// for the same (nodeId, metric) pair, otherwise claims a free slot.
    ///
    /// `ts` is honoured when it is real. SensorManager::tickFiltered() stamps
    /// a reading only when its timestamp is zero, so a node that knows when it
    /// measured keeps that time all the way to storage.
    ///
    /// For THIS path that rarely matters — the ESP8266 reference node has no
    /// clock and sends 0, and a live reading is current by definition. It
    /// matters for putHistorical() below, which is the path a node uses to
    /// hand over readings it buffered through an outage. Those are the ones
    /// whose times would otherwise all collapse onto the moment they arrived.
    ///
    /// Non-finite values are rejected so a garbage sample cannot poison a
    /// series or occupy the last free slot. Returns false when the table is
    /// full and the pair is not already present.
    bool put(const char* nodeId, const char* metric, float value,
             const char* unit, uint32_t ts);

    /// Record one metric that was measured EARLIER and is only arriving now.
    ///
    /// The difference from put() is the whole point: put() is a mailbox slot
    /// and overwrites, which is right for a node reporting faster than the
    /// collector ticks. It is wrong for a node that buffered fifteen readings
    /// through an outage and is handing them over — those are fifteen distinct
    /// measurements, and overwriting them into one slot loses fourteen.
    ///
    /// So these queue instead. `ts` is authoritative here and is NOT stamped
    /// over downstream: SensorManager keeps a timestamp a plugin supplied.
    /// A reading with no usable clock is refused rather than queued, because a
    /// backdated reading whose date is wrong is worse than a gap.
    ///
    /// Returns false when the queue is full, in which case the OLDEST entry is
    /// dropped to make room — losing the start of an outage beats losing the
    /// end of it, the same rule the node applies to its own buffer.
    bool putHistorical(const char* nodeId, const char* metric, float value,
                       const char* unit, uint32_t ts);

    /// How many queued historical readings are still waiting to be drained.
    int historyPending() const;

    /// Copy every metric held for `nodeId` into `out`, up to `maxOut`.
    /// Fills metric/value/unit/timestamp/quality only — SensorManager
    /// overwrites sensorId and sensorType from the plugin instance.
    ///
    /// Latest values come FIRST and queued history fills whatever room is
    /// left. That ordering is deliberate and not arbitrary: history drains a
    /// few readings per tick, so putting it first would leave the live
    /// dashboard showing nothing current until an outage's backlog had
    /// cleared — minutes of a frozen display to deliver readings nobody is
    /// waiting on. Behind the current value, it trickles in unnoticed.
    ///
    /// `staleAfterMs` marks entries older than that as QUALITY_ERROR
    /// instead of dropping them: a dashboard showing a stale outdoor
    /// temperature with an age is more useful than one showing a gap, and
    /// ProcessingTask is what decides whether an errored reading is stored.
    /// Pass 0 to disable the staleness check.
    /// NOT const, and that is the honest signature now: draining REMOVES the
    /// queued history it returns. It was const while the mailbox only ever
    /// copied out latest values; keeping the qualifier and making the members
    /// mutable would have hidden a real change of meaning behind a keyword.
    int drain(const char* nodeId, SensorReading* out, int maxOut,
              uint32_t staleAfterMs);

    /// millis() since the most recent put() for `nodeId`, or UINT32_MAX when
    /// the node has never reported. Used by the diagnostics endpoint and the
    /// Kindle dashboard to show "last seen".
    ///
    /// UINT32_MAX is a SENTINEL, not an age. A caller that passes it on to a
    /// display or an API without testing for it publishes 4,294,967,295 ms —
    /// forty-nine days — as though it were a measurement.
    uint32_t ageMsForNode(const char* nodeId) const;

    /// Number of distinct nodes that have reported at least once.
    int nodeCount() const;

    /// Writes the id of node `index` (0-based, in slot order) into `out`.
    /// Returns false when `index` is past the last known node.
    bool nodeIdAt(int index, char* out, size_t outLen) const;

    /// The latest value the mailbox holds for each of `nodeId`'s metrics.
    ///
    /// A READ, unlike drain(): nothing is consumed, no history is touched, and
    /// the sensor plugin that owns this node still receives every reading on
    /// its own tick. That is what makes it safe to call from a web handler.
    ///
    /// `staleAfterMs` is the age past which a value is reported as
    /// QUALITY_ERROR rather than QUALITY_GOOD — the same rule drain() applies,
    /// because a mailbox re-emits an entry that is never refilled and a dead
    /// node would otherwise read exactly like a live one. 0 means "do not
    /// judge", and every reading comes back QUALITY_GOOD.
    ///
    /// Returns how many readings were written, at most `maxOut`.
    int peekLatest(const char* nodeId, SensorReading* out, int maxOut,
                   uint32_t staleAfterMs = 0) const;

private:
    /// Queued historical readings, across all nodes.
    ///
    /// Sized for one node emptying a full buffer: fourteen samples of four
    /// metrics is fifty-six, and 64 × 40 bytes is 2.5 KB of static RAM on a
    /// part with 320 KB. Two nodes recovering at the same moment will drop the
    /// oldest of the two backlogs, which is the right thing to lose.
#ifndef REMOTE_HISTORY_SLOTS
#  define REMOTE_HISTORY_SLOTS 64
#endif
    static constexpr int MAX_HISTORY = REMOTE_HISTORY_SLOTS;

    struct Hist {
        char     nodeId[MAX_NODE_ID];
        char     metric[MAX_METRIC];
        char     unit[MAX_UNIT];
        float    value;
        uint32_t ts;
    };

    struct Entry {
        char     nodeId[MAX_NODE_ID];
        char     metric[MAX_METRIC];
        char     unit[MAX_UNIT];
        float    value;
        uint32_t ts;         // node-supplied epoch seconds, 0 = none
        uint32_t rxMillis;   // local millis() at receipt
        bool     used;
    };

    /// Copies the distinct node ids into `out` and returns how many. The
    /// spinlock covers only the linear copy; the O(n²) dedup runs after it
    /// is released.
    int _snapshotNodes(char (&out)[MAX_ENTRIES][MAX_NODE_ID]) const;

    Entry _e[MAX_ENTRIES] = {};
    Hist  _h[MAX_HISTORY] = {};
    int   _hHead = 0;      ///< next to drain
    int   _hCount = 0;     ///< occupied slots
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern RemoteIngest remoteIngest;
