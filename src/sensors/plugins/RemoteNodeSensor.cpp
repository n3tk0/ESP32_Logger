#include "RemoteNodeSensor.h"

#ifdef FEATURE_REMOTE_NODES

#include <string.h>
#include "../RemoteIngest.h"

bool RemoteNodeSensor::init(JsonObjectConst config) {
    // Default the node id to the sensor id: naming both after the location
    // is the expected setup, and it removes the commonest configuration
    // mistake (a node posting under a name nothing is listening for).
    const char* node = config["node"] | getId();
    strncpy(_node, node ? node : "", sizeof(_node) - 1);
    _node[sizeof(_node) - 1] = '\0';

    _staleAfterMs   = config["stale_after_ms"]   | 600000UL;
    _readIntervalMs = config["read_interval_ms"] | 30000UL;

    if (_node[0] == '\0') {
        Serial.printf("[%s.remote] init refused: no node id\n", getId());
        return false;
    }

    // Nothing to probe — there is no bus and no device. Success here means
    // "configured", not "the node is alive"; liveness shows up as reading
    // quality once (or if) the node starts posting.
    Serial.printf("[%s.remote] listening for node \"%s\" (stale after %lu ms)\n",
                  getId(), _node, (unsigned long)_staleAfterMs);
    return true;
}

int RemoteNodeSensor::readAll(SensorReading* out, int maxOut) {
    const int n = remoteIngest.drain(_node, out, maxOut, _staleAfterMs);

    // Remember the metric names for getMetrics(). Rebuilt from each drain so
    // a node that starts reporting humidity mid-life (BMP280 swapped for a
    // BME280) shows up without a reboot.
    if (n > 0) {
        int keep = (n < MAX_METRICS) ? n : MAX_METRICS;
        for (int i = 0; i < keep; i++) {
            strncpy(_metricNames[i], out[i].metric, sizeof(_metricNames[i]) - 1);
            _metricNames[i][sizeof(_metricNames[i]) - 1] = '\0';
        }
        _metricCount = keep;
    }

    return n;
}

bool RemoteNodeSensor::read(SensorReading& out) {
    // Single-metric path, only reached if something calls read() directly.
    return readAll(&out, 1) == 1;
}

int RemoteNodeSensor::getMetrics(const char** out, int maxOut) const {
    const int n = (_metricCount < maxOut) ? _metricCount : maxOut;
    for (int i = 0; i < n; i++) out[i] = _metricNames[i];
    return n;
}

#endif  // FEATURE_REMOTE_NODES
