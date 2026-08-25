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
    /// `ts` is the node's own Unix timestamp, or 0 when it has no clock —
    /// the collector substitutes its own time at drain in that case, which
    /// is the common case for an ESP8266 with no RTC and no NTP.
    ///
    /// Non-finite values are rejected so a garbage sample cannot poison a
    /// series or occupy the last free slot. Returns false when the table is
    /// full and the pair is not already present.
    bool put(const char* nodeId, const char* metric, float value,
             const char* unit, uint32_t ts);

    /// Copy every metric held for `nodeId` into `out`, up to `maxOut`.
    /// Fills metric/value/unit/timestamp/quality only — SensorManager
    /// overwrites sensorId and sensorType from the plugin instance.
    ///
    /// `staleAfterMs` marks entries older than that as QUALITY_ERROR
    /// instead of dropping them: a dashboard showing a stale outdoor
    /// temperature with an age is more useful than one showing a gap, and
    /// ProcessingTask is what decides whether an errored reading is stored.
    /// Pass 0 to disable the staleness check.
    int drain(const char* nodeId, SensorReading* out, int maxOut,
              uint32_t staleAfterMs) const;

    /// millis() since the most recent put() for `nodeId`, or UINT32_MAX when
    /// the node has never reported. Used by the diagnostics endpoint and the
    /// Kindle dashboard to show "last seen".
    uint32_t ageMsForNode(const char* nodeId) const;

    /// Number of distinct nodes that have reported at least once.
    int nodeCount() const;

    /// Writes the id of node `index` (0-based, in slot order) into `out`.
    /// Returns false when `index` is past the last known node.
    bool nodeIdAt(int index, char* out, size_t outLen) const;

private:
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
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern RemoteIngest remoteIngest;
