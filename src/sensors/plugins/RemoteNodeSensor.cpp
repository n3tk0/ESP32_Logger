#include "RemoteNodeSensor.h"

// See TrendRing.cpp: the guard below needs setup.h, which ISensor.h's include
// chain does not reach.
#include "../../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <string.h>
#include "../RemoteIngest.h"

bool RemoteNodeSensor::init(JsonObjectConst config) {
    // NOTHING HERE TOUCHES _enabled, AND THAT IS THE FIX.
    //
    // It used to default to false, with every plugin expected to turn itself
    // on in init(). Twenty did; this one did not, so a remote sensor was
    // created disabled: it appeared in /api/sensors as
    // {"enabled":false,"status":"disabled"}, tickFiltered() skipped it,
    // drain() was therefore never called, and it never grew a single metric —
    // while the node itself was plainly alive on the Remote-nodes page, which
    // reads the ingest mailbox directly. The one screen that could have
    // explained it was the one screen that looked fine.
    //
    // The default is true now (ISensor.h), because SensorManager::loadAndInit()
    // skips every config entry whose "enabled" is false before constructing
    // anything: a sensor that exists is one the user asked for. Reading the
    // key again here would be reading a question already answered, and this
    // file is the one everybody copies from — leaving the line in is how the
    // per-plugin obligation gets reintroduced.

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
    //
    // DISTINCT names, because drain() stopped returning distinct readings.
    // It now appends the backfill queue after the live values, so an outage
    // backlog hands back temperature, humidity, pressure, temperature,
    // humidity, … — and copying that verbatim published a sensor whose metric
    // list repeated itself into /api/sensors and into MQTT Home Assistant
    // discovery, where each repeat is another entity for the same reading.
    if (n > 0) {
        int keep = 0;
        for (int i = 0; i < n && keep < MAX_METRICS; i++) {
            bool seen = false;
            for (int j = 0; j < keep; j++) {
                if (strcmp(_metricNames[j], out[i].metric) == 0) { seen = true; break; }
            }
            if (seen) continue;
            strncpy(_metricNames[keep], out[i].metric, sizeof(_metricNames[keep]) - 1);
            _metricNames[keep][sizeof(_metricNames[keep]) - 1] = '\0';
            keep++;
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
