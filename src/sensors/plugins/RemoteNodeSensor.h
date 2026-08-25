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
//     "read_interval_ms": 30000
//   }
//
// `node` defaults to `id` when omitted, which is the common case — name the
// node after the place and the sensor id after the same place.
//
// The staleness window should be a comfortable multiple of the node's own
// posting interval: a node reporting every 60 s and a 600 s window tolerates
// nine missed posts before its readings start being marked QUALITY_ERROR.
// Too tight and a single dropped WiFi packet flags the station as failed;
// too loose and a dead node keeps publishing a plausible frozen value.
// ============================================================================
#pragma once

#include "../ISensor.h"

class RemoteNodeSensor : public ISensor {
public:
    bool init(JsonObjectConst config) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

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
    static constexpr int MAX_METRICS = 8;
    mutable char _metricNames[MAX_METRICS][16] = {};
    mutable int  _metricCount = 0;
};
