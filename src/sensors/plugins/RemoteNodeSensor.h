// ============================================================================
// src/sensors/plugins/RemoteNodeSensor.h
//
// A sensor that is not wired to this board.
//
// Its values arrive over the network from a remote node (see node/ for the
// ESP8266 reference implementation) and wait in RemoteIngest until this
// plugin's tick drains them. From SensorManager's point of view it behaves
// like any other multi-metric plugin, which is the whole point: calibration,
// filtering, the ReadingCache feed, the ring buffer, MQTT/HTTP export and
// the dashboards all work on remote readings without knowing they are remote.
//
// Config:
//   {
//     "id":              "outdoor",       // what this station is called here
//     "type":            "remote",
//     "enabled":         true,
//     "node":            "balcony",       // must match the node's own id
//     "stale_after_ms":  600000,          // 0 disables the freshness check
//     "read_interval_ms": 30000,
//     "calibration":     { "temperature": { "offset": -0.5, "scale": 1.0 } }
//   }
//
// `calibration` is keyed by the metric name the node sends, since a node's
// metric list is whatever it reports rather than fixed by a driver. It is
// applied here, on the collector, so WiFi and ESP-NOW nodes are corrected
// the same way without the node firmware or the radio protocol knowing.
//
// Besides what the node sends, it publishes `dew_point` (from temperature and
// humidity) and `iaq` (from gas_resistance and humidity), the way the wired
// BME280/BME688 plugins do — see _derive().
//
// `node` defaults to `id` when omitted, which is the common case — name the
// node after the place and the sensor id after the same place.
//
// The staleness window should be a comfortable multiple of the node's own
// posting interval: a node reporting every 60 s and a 600 s window tolerates
// nine missed posts before its readings start being marked QUALITY_ERROR.
// Too tight and a single dropped WiFi packet flags the station as failed;
// too loose and a dead node keeps publishing a plausible frozen value.
// A metric the node sends less often than that (a sensor with its own, longer
// interval_s) gets two and a half times its own gap instead — see
// RemoteIngest::staleLimitMs().
// ============================================================================
#pragma once

#include "../ISensor.h"
#include "../../utils/GasIaq.h"
#include "../../utils/IaqBaselineStore.h"

class RemoteNodeSensor : public ISensor {
public:
    bool init(JsonObjectConst config) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    /// What readAll() returns minus the queued history: the live values, as
    /// corrected, and the derived ones. Consumes nothing — for
    /// /api/sensors/read_now, where history handed back would be shown once
    /// and never stored.
    int  readLatest(SensorReading* out, int maxOut);

    const char* getType() const override { return "remote"; }
    const char* getName() const override { return "Remote node"; }

    uint32_t getReadIntervalMs() const override { return _readIntervalMs; }
    int      getMetrics(const char** out, int maxOut) const override;

    // A node that has not posted yet — or at all — makes readAll() return 0.
    // That is the network's state, not a fault of this board's hardware, and
    // counting it would bury a genuinely broken local sensor in the health
    // totals. A node that HAS reported and then went quiet is not silent
    // here: drain() keeps returning its last values marked QUALITY_ERROR
    // once they age past stale_after_ms, which is what surfaces on the
    // dashboard and in the exporters.
    bool countEmptyReadAsError() const override { return false; }

private:
    char     _node[17]      = {0};
    uint32_t _staleAfterMs  = 600000;
    uint32_t _readIntervalMs = 30000;

    // Metric names seen from this node, remembered so getMetrics() can
    // answer before the next drain. Pointers handed out must stay valid,
    // so these are owned storage rather than pointers into RemoteIngest.
    // A node's own twelve (nodecfg::MAX_METRICS), an ESP-NOW node's three
    // battery metrics and the derived dew_point and iaq: 17, and room over.
    static constexpr int MAX_METRICS = 20;
    mutable char _metricNames[MAX_METRICS][16] = {};
    mutable int  _metricCount = 0;

    // Per-metric corrections from config["calibration"], matched by name.
    struct MetricCal { char metric[16]; CalibrationAxis axis; };
    MetricCal _cal[MAX_METRICS] = {};
    int       _calCount = 0;

    void _calibrate(SensorReading* out, int n) const;
    /// Append dew_point and iaq to the n live readings in `out`; returns the
    /// new count.
    int  _derive(SensorReading* out, int n, int maxOut, float rawTemp, float rawGas);

    // iaq for this node's BME688: the baseline, and the last gas reading it
    // was fed (by its time and value) so a mailbox repeat is not fed twice.
    GasIaq   _iaq;
    float    _iaqLast  = 0.0f;
    float    _iaqGas   = 0.0f;
    uint32_t _iaqTs    = 0;
    bool     _iaqValid = false;

    // Kept across restarts, one file per node. The heater settings stored
    // with it are the ones node firmware runs: BME688_Mini::begin()'s
    // setGasHeater(320, 150). Keep the two in step; changing these makes
    // every saved node baseline start over.
    static constexpr int NODE_HEATER_TEMP   = 320;
    static constexpr int NODE_HEATER_DUR_MS = 150;
    IaqBaselineStore::Saver _saver;
    void _loadBaseline();
    void _maybeSaveBaseline();
};
