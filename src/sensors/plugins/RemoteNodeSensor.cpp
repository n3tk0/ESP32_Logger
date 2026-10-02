#include "RemoteNodeSensor.h"

// See TrendRing.cpp: the guard below needs setup.h, which ISensor.h's include
// chain does not reach.
#include "../../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <math.h>
#include <string.h>
#include "../RemoteIngest.h"
#include "../../utils/Psychrometrics.h"
#include "../../utils/IaqBaselineStore.h"

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

    _calCount = 0;
    JsonObjectConst cal = config["calibration"];
    for (JsonPairConst kv : cal) {
        if (_calCount >= MAX_METRICS) break;
        MetricCal& c = _cal[_calCount];
        strncpy(c.metric, kv.key().c_str(), sizeof(c.metric) - 1);
        c.metric[sizeof(c.metric) - 1] = '\0';
        c.axis = CalibrationAxis();
        c.axis.load(cal, c.metric);
        // An identity entry is a form left at its defaults: nothing to do.
        if (c.axis.offset == 0.0f && c.axis.scale == 1.0f) continue;
        _calCount++;
    }

    if (_node[0] == '\0') {
        Serial.printf("[%s.remote] init refused: no node id\n", getId());
        return false;
    }

    // A new configuration starts the iaq baseline from a clean slate; only
    // this node's own saved one (under the heater settings node firmware
    // runs) is restored. Another node id is another file.
    _iaq      = GasIaq();
    _iaqValid = false;
    _loadBaseline();

    // Nothing to probe — there is no bus and no device. Success here means
    // "configured", not "the node is alive"; liveness shows up as reading
    // quality once (or if) the node starts posting.
    Serial.printf("[%s.remote] listening for node \"%s\" (stale after %lu ms)\n",
                  getId(), _node, (unsigned long)_staleAfterMs);
    return true;
}

// The first reading in out[0..n) named `metric`, or -1.
static int findMetric(const SensorReading* out, int n, const char* metric) {
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].metric, metric) == 0) return i;
    return -1;
}

void RemoteNodeSensor::_calibrate(SensorReading* out, int n) const {
    // drain() copies out of the mailbox, so correcting the copy here never
    // compounds on a value handed back again (a stale repeat, a backlog).
    for (int i = 0; i < n && _calCount > 0; i++) {
        for (int j = 0; j < _calCount; j++) {
            if (strcmp(_cal[j].metric, out[i].metric) == 0) {
                out[i].value = _cal[j].axis.apply(out[i].value);
                break;
            }
        }
    }
}

int RemoteNodeSensor::_derive(SensorReading* out, int n, int maxOut,
                              float rawTemp, float rawGas) {
    // dew_point and iaq, as the collector's own BME280/BME688 plugins publish
    // them, worked out here from what the node sent rather than sent by it:
    // they cost the node, the radio and the node's metric budget nothing, and
    // the formula is the one a wired sensor uses. A node that does send its
    // own is left alone.
    const int t = findMetric(out, n, "temperature");
    const int h = findMetric(out, n, "humidity");
    const int g = findMetric(out, n, "gas_resistance");

    // Dew point pairs the RH with the temperature it was measured AT — the
    // raw one, as BME280Sensor and BME688Sensor do: a corrected temperature
    // would bake its correction into the one figure meant to be free of it.
    if (t >= 0 && h >= 0 && n < maxOut && findMetric(out, n, "dew_point") < 0) {
        const float dew = Psychro::dewPointC(rawTemp, out[h].value);
        if (isfinite(dew)) {
            SensorReading& r = out[n++];
            r = SensorReading();
            strncpy(r.metric, "dew_point", sizeof(r.metric) - 1);
            strncpy(r.unit, "C", sizeof(r.unit) - 1);
            r.value     = dew;
            r.timestamp = out[t].timestamp;
            r.quality   = (out[t].quality == QUALITY_GOOD) ? out[h].quality : out[t].quality;
        }
    }

    // The baseline moves with every reading it is fed, and the mailbox hands
    // the same gas value back on every tick until the node sends a new one,
    // so only a new one (another time or another value) is folded in. A
    // repeat gets the index that reading produced.
    if (g >= 0 && h >= 0 && n < maxOut && findMetric(out, n, "iaq") < 0 && isfinite(rawGas)) {
        if (!_iaqValid || out[g].timestamp != _iaqTs || rawGas != _iaqGas) {
            _iaqLast  = _iaq.update(out[h].value, rawGas);
            _iaqTs    = out[g].timestamp;
            _iaqGas   = rawGas;
            _iaqValid = true;
        }
        SensorReading& r = out[n++];
        r = SensorReading();
        strncpy(r.metric, "iaq", sizeof(r.metric) - 1);
        r.value     = _iaqLast;
        r.timestamp = out[g].timestamp;
        r.quality   = out[g].quality;
    }
    return n;
}

int RemoteNodeSensor::readLatest(SensorReading* out, int maxOut) {
    int n = remoteIngest.drainLatest(_node, out, maxOut, _staleAfterMs);

    // The derived metrics want the temperature and gas resistance as the
    // node measured them, so take those before the corrections go on.
    const int t = findMetric(out, n, "temperature");
    const int g = findMetric(out, n, "gas_resistance");
    const float rawTemp = t >= 0 ? out[t].value : NAN;
    const float rawGas  = g >= 0 ? out[g].value : NAN;
    _calibrate(out, n);
    return _derive(out, n, maxOut, rawTemp, rawGas);
}

// ---------------------------------------------------------------------------
// The iaq baseline survives a collector restart (utils/IaqBaselineStore.h),
// one file per node id. The node's heater is never cold on the collector's
// account, so unlike BME688Sensor there is no warm-up to wait out here.
void RemoteNodeSensor::_loadBaseline() {
    char path[32];
    IaqBaselineStore::path("iaqn", _node, path, sizeof(path));
    _iaq.baseline = IaqBaselineStore::load(path, NODE_HEATER_TEMP, NODE_HEATER_DUR_MS, getId());
    _saver.reset(millis(), _iaq.baseline);
}

void RemoteNodeSensor::_maybeSaveBaseline() {
    if (!_iaqValid) return;
    const uint32_t now = millis();
    if (!_saver.due(now, _iaq.baseline)) return;
    char path[32];
    IaqBaselineStore::path("iaqn", _node, path, sizeof(path));
    // The sensor task reads every sensor in turn: a busy fsMutex is not
    // worth stalling it for, the saver retries in five minutes.
    const bool ok = IaqBaselineStore::save(path, NODE_HEATER_TEMP, NODE_HEATER_DUR_MS,
                                           _iaq.baseline, 50);
    _saver.done(now, _iaq.baseline, ok);
}

int RemoteNodeSensor::readAll(SensorReading* out, int maxOut) {
    int n = readLatest(out, maxOut);

    // Queued history last, in whatever room is left — behind the live values
    // and their derived ones, so an outage's backlog never pushes the current
    // reading out of a tick.
    const int h = remoteIngest.drainHistory(_node, out + n, maxOut - n);
    _calibrate(out + n, h);
    n += h;

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
