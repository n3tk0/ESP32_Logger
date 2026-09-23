// ============================================================================
// src/nodecfg/MetricCatalog.h
//
// Every metric a configurable node can publish: its wire id, its name on the
// collector, its unit — and, from a NodeConfig, the exact list of metrics that
// config produces.
//
// WHY ONE LIST AND NOT ONE PER FIRMWARE
// -------------------------------------
// Four readers need the same answer:
//
//   the validator    counts metrics against the 8-per-tick budget and refuses
//                    two sensors that would publish under the same name;
//   the WiFi node    names each reading it POSTs;
//   the ESP-NOW node packs (metric id, index) pairs into a DATA2 frame
//                    instead of names, because names do not fit in 250 bytes;
//   the collector    turns those pairs back into names.
//
// The last two are the dangerous pair. If the node's idea of "metric 13,
// index 2" and the collector's differ, readings land under the wrong name and
// nothing anywhere reports an error. So there is one function —
// listMetrics() — that walks a config in a fixed order, and both ends call it
// on the same config: the node on its applied one, the collector on the one
// the node last reported.
//
// The ids are docs/NODE_CONFIG.md §5 and are a wire format: never renumber,
// only append.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include "NodeConfig.h"
#include "../espnow/EspNowProto.h"

namespace nodecfg {

enum MetricId : uint8_t {
    M_NONE            = 0,
    M_TEMPERATURE     = 1,
    M_HUMIDITY        = 2,
    M_PRESSURE        = 3,
    M_PRESSURE_SEA    = 4,
    M_GAS_RESISTANCE  = 5,
    M_LUX             = 6,
    M_PM25            = 7,
    M_PM10            = 8,
    M_RAIN_RATE       = 9,
    M_RAIN_TOTAL      = 10,
    M_FLOW_RATE       = 11,
    M_FLOW_TOTAL      = 12,
    M_PROBE_TEMP      = 13,   ///< index = probe ordinal, see listMetrics()
    M_BATTERY_VOLTAGE = 14,   ///< ESP-NOW only, never counted in the budget
};
static const uint8_t METRIC_ID_MAX = 14;

struct MetricInfo {
    uint8_t     id;
    const char* name;
    const char* unit;
};

/// Indexed by MetricId. The names and units are the ones the collector's own
/// sensor plugins and the WiFi node already publish (temperature in "C",
/// gas_resistance in "Ohm", …) — a remote reading must be indistinguishable
/// from a wired one downstream, or every chart and alert keyed by unit splits
/// in two.
static const MetricInfo METRICS[METRIC_ID_MAX + 1] = {
    { M_NONE,            "",                "" },
    { M_TEMPERATURE,     "temperature",     "C" },
    { M_HUMIDITY,        "humidity",        "%" },
    { M_PRESSURE,        "pressure",        "hPa" },
    { M_PRESSURE_SEA,    "pressure_sea",    "hPa" },
    { M_GAS_RESISTANCE,  "gas_resistance",  "Ohm" },
    { M_LUX,             "lux",             "lx" },
    { M_PM25,            "pm25",            "ug/m3" },
    { M_PM10,            "pm10",            "ug/m3" },
    { M_RAIN_RATE,       "rain_rate",       "mm/h" },
    { M_RAIN_TOTAL,      "rain_total",      "mm" },
    { M_FLOW_RATE,       "flow_rate",       "L/min" },
    { M_FLOW_TOTAL,      "flow_total",      "L" },
    { M_PROBE_TEMP,      "probe_temp",      "C" },
    { M_BATTERY_VOLTAGE, "battery_voltage", "V" },
};

/// The catalogue entry for `id`, or nullptr for an id this firmware does not
/// know — which the collector must treat as "drop this value", not as an
/// error for the frame: a newer node may publish a metric an older collector
/// has no name for, and its other values are still good.
static inline const MetricInfo* metricInfo(uint8_t id) {
    if (id == M_NONE || id > METRIC_ID_MAX) return nullptr;
    return &METRICS[id];
}

/// The id for a catalogue name, or M_NONE.
static inline uint8_t metricIdByName(const char* name) {
    if (!name || !*name) return M_NONE;
    for (uint8_t i = 1; i <= METRIC_ID_MAX; i++) {
        if (strcmp(METRICS[i].name, name) == 0) return i;
    }
    return M_NONE;
}

// ---------------------------------------------------------------------------
// Per sensor type
// ---------------------------------------------------------------------------

/// The metric ids one entry of `type` produces, in the order it produces them.
/// For ds18b20 this is the single id M_PROBE_TEMP; the entry produces it
/// `count` times. For pulse the ids depend on the mode, so the caller passes it.
/// Returns the number of ids written (at most 5).
static inline uint8_t sensorTypeMetricIds(SensorType type, PulseMode mode, uint8_t out[5]) {
    uint8_t n = 0;
    switch (type) {
        case SensorType::Bme688:
        case SensorType::Bmx280:
            out[n++] = M_TEMPERATURE;
            out[n++] = M_HUMIDITY;
            out[n++] = M_PRESSURE;
            out[n++] = M_PRESSURE_SEA;
            if (type == SensorType::Bme688) out[n++] = M_GAS_RESISTANCE;
            break;
        case SensorType::Ds18b20:
            out[n++] = M_PROBE_TEMP;
            break;
        case SensorType::Bh1750:
            out[n++] = M_LUX;
            break;
        case SensorType::Sds011:
            out[n++] = M_PM25;
            out[n++] = M_PM10;
            break;
        case SensorType::Pulse:
            if (mode == PulseMode::Flow) {
                out[n++] = M_FLOW_RATE;
                out[n++] = M_FLOW_TOTAL;
            } else {
                out[n++] = M_RAIN_RATE;
                out[n++] = M_RAIN_TOTAL;
            }
            break;
        default:
            break;
    }
    return n;
}

/// How many metrics one entry costs against MAX_METRICS.
///
/// pressure_sea counts even when altitude_m is 0 and it is not published —
/// §1.1. Counting what MIGHT be published is what keeps setting an altitude
/// later from silently pushing a config over the budget it was accepted under.
static inline uint8_t sensorMetricCount(const SensorCfg& s) {
    uint8_t ids[5];
    const uint8_t n = sensorTypeMetricIds(s.type, s.mode, ids);
    if (s.type == SensorType::Ds18b20) return s.count;
    return n;
}

/// Metric budget of a whole config (battery_voltage excluded).
static inline uint16_t configMetricCount(const NodeConfig& c) {
    uint16_t n = 0;
    const uint8_t k = c.sensor_count <= MAX_SENSORS ? c.sensor_count : MAX_SENSORS;
    for (uint8_t i = 0; i < k; i++) n += sensorMetricCount(c.sensors[i]);
    return n;
}

// ---------------------------------------------------------------------------
// A config's metrics, in wire order
// ---------------------------------------------------------------------------

/// Longest metric name the collector stores: SensorReading::metric is
/// char[16], so fifteen characters.
static const size_t METRIC_NAME_CAP = 16;

/// One metric a config publishes.
struct MetricSlot {
    uint8_t     id;         ///< MetricId
    uint8_t     index;      ///< DATA2 `index`: probe ordinal for M_PROBE_TEMP, else 0
    uint8_t     sensor;     ///< which sensors[] entry produces it
    uint8_t     probe;      ///< ds18b20: probe number within that entry
    bool        needsAltitude;   ///< pressure_sea: published only if altitude_m != 0
    char        name[METRIC_NAME_CAP];
    const char* unit;
};

/// Most slots a VALID config produces: every counted metric plus battery.
/// (An unvalidated one can list more; listMetrics() counts them regardless.)
static const uint8_t MAX_METRIC_SLOTS = MAX_METRICS + 1;

/// A DS18B20 probe's metric name: `base` for probe 0, `base_N` after that.
static inline void probeMetricName(char name[METRIC_NAME_CAP], const char* base, unsigned p) {
    copyStr(name, METRIC_NAME_CAP, base);
    if (p == 0) return;
    strAppend(name, METRIC_NAME_CAP, "_");
    strAppendUint(name, METRIC_NAME_CAP, p);
}

/// List every metric `c` publishes, in the one order both ends of the radio
/// agree on: sensors[] in order, each entry's metrics in catalogue order,
/// then battery_voltage last when `withBattery`.
///
/// NAMING DS18B20 PROBES. Probe N of an entry is "<metric>" for N = 0 and
/// "<metric>_N" after that — the WiFi node's historical naming. On the radio
/// the id is always M_PROBE_TEMP and `index` is the probe's ordinal across
/// ALL ds18b20 entries in list order (entry A's two probes are 0 and 1, entry
/// B's first is 2), which is what lets the collector name a probe of the
/// second bus correctly: index 2 is entry B, probe 0, and gets entry B's
/// `metric`. §5's "the name suffix _N is added for N >= 1" is about N within
/// an entry.
///
/// Every counted slot is listed even past `max`: the return value is the
/// total, so a caller with a small array can still tell it was cut short.
/// Slots beyond MAX_METRIC_SLOTS never exist in a config that validated.
static inline uint8_t listMetrics(const NodeConfig& c, MetricSlot* out, uint8_t max,
                                  bool withBattery) {
    uint8_t n = 0;
    uint8_t probeOrdinal = 0;
    const uint8_t k = c.sensor_count <= MAX_SENSORS ? c.sensor_count : MAX_SENSORS;
    for (uint8_t si = 0; si < k; si++) {
        const SensorCfg& s = c.sensors[si];
        uint8_t ids[5];
        const uint8_t m = sensorTypeMetricIds(s.type, s.mode, ids);
        if (s.type == SensorType::Ds18b20) {
            const uint8_t cnt = s.count <= DS_MAX_COUNT ? s.count : DS_MAX_COUNT;
            for (uint8_t p = 0; p < cnt; p++, probeOrdinal++, n++) {
                if (n >= max || !out) continue;
                MetricSlot& o = out[n];
                o.id = M_PROBE_TEMP;
                o.index = probeOrdinal;
                o.sensor = si;
                o.probe = p;
                o.needsAltitude = false;
                o.unit = METRICS[M_PROBE_TEMP].unit;
                const char* base = s.metric[0] ? s.metric : METRICS[M_PROBE_TEMP].name;
                probeMetricName(o.name, base, p);
            }
            continue;
        }
        for (uint8_t j = 0; j < m; j++, n++) {
            if (n >= max || !out) continue;
            MetricSlot& o = out[n];
            o.id = ids[j];
            o.index = 0;
            o.sensor = si;
            o.probe = 0;
            o.needsAltitude = (ids[j] == M_PRESSURE_SEA);
            o.unit = METRICS[ids[j]].unit;
            copyStr(o.name, sizeof(o.name), METRICS[ids[j]].name);
        }
    }
    if (withBattery) {
        if (n < max && out) {
            MetricSlot& o = out[n];
            o.id = M_BATTERY_VOLTAGE;
            o.index = 0;
            o.sensor = 0xFF;
            o.probe = 0;
            o.needsAltitude = false;
            o.unit = METRICS[M_BATTERY_VOLTAGE].unit;
            copyStr(o.name, sizeof(o.name), METRICS[M_BATTERY_VOLTAGE].name);
        }
        n++;
    }
    return n;
}

/// The collector's half of DATA2: the name and unit of (id, index) under the
/// config the node reported. Writes into `name` (METRIC_NAME_CAP) and returns
/// the unit, or nullptr when the pair means nothing — an unknown id, or a
/// probe index past the config's probes.
///
/// `c` may be null: a node that has not reported its config yet (it is sent on
/// the first wake after boot, so this is the window after a collector
/// restart). Then a probe gets the default naming — probe_temp, probe_temp_N
/// with N the ordinal — which is exactly right for the single-entry node that
/// is the common case, and every other metric has only one possible name.
static inline const char* metricNameFor(const NodeConfig* c, uint8_t id, uint8_t index,
                                        char name[METRIC_NAME_CAP]) {
    const MetricInfo* mi = metricInfo(id);
    if (!mi) return nullptr;
    if (id != M_PROBE_TEMP) {
        copyStr(name, METRIC_NAME_CAP, mi->name);
        return mi->unit;
    }
    if (!c) {
        probeMetricName(name, mi->name, index);
        return mi->unit;
    }
    // The same walk as listMetrics(), without materialising the list: find
    // which ds18b20 entry the ordinal falls in. The host test checks the two
    // agree for every slot of every config it builds.
    unsigned ordinal = 0;
    const uint8_t k = c->sensor_count <= MAX_SENSORS ? c->sensor_count : MAX_SENSORS;
    for (uint8_t si = 0; si < k; si++) {
        const SensorCfg& s = c->sensors[si];
        if (s.type != SensorType::Ds18b20) continue;
        const uint8_t cnt = s.count <= DS_MAX_COUNT ? s.count : DS_MAX_COUNT;
        if (index < ordinal + cnt) {
            const unsigned p = index - ordinal;
            const char* base = s.metric[0] ? s.metric : mi->name;
            probeMetricName(name, base, p);
            return mi->unit;
        }
        ordinal += cnt;
    }
    return nullptr;
}

// A DATA2 sample carries every counted metric plus the battery voltage.
static_assert(MAX_METRICS + 1 <= EN_DATA2_MAX_VALUES,
              "a DATA2 sample must hold a full config's metrics plus battery_voltage");
static_assert(METRIC_NAME_CAP == 16, "SensorReading::metric is char[16] on the collector");

}  // namespace nodecfg
