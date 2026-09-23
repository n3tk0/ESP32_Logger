// ============================================================================
// node_common/NodeSensors.h
//
// The runtime sensor layer shared by both node firmwares: the ESP8266 WiFi
// node (node/) and the XIAO ESP32-C3 ESP-NOW node (node_espnow/).
//
// WHY A THIRD DIRECTORY AND NOT src/
// ----------------------------------
// src/ is the collector's source tree: a .cpp there is compiled into the
// collector's firmware, which has its own plugin-based sensor stack and does
// not want this one. The nodes reach this directory through their existing
// `-I..` build flag, and each compiles NodeSensors.cpp through a one-line
// wrapper in its own src/ (PlatformIO only builds what is under src/).
//
// WHAT CHANGED FROM node/src/sensors.h
// ------------------------------------
// The sensor set used to be chosen at build time (NODE_SENSOR_*). It is now
// the `sensors` list of a nodecfg::NodeConfig (docs/NODE_CONFIG.md §1.1), so
// every driver is compiled into every build and only the listed ones are
// brought up. The metric names, units and order come from
// src/nodecfg/MetricCatalog.h (listMetrics), so the WiFi node's JSON, the
// ESP-NOW node's DATA2 frame and the collector's decoding cannot disagree.
//
// This header is the contract between the two node firmwares; the
// implementation (NodeSensors.cpp) must build on both ESP8266 and ESP32-C3.
// ============================================================================
#pragma once

#include <stdint.h>

#include "src/nodecfg/NodeConfig.h"
#include "src/nodecfg/MetricCatalog.h"

/// One measurement of one metric.
struct NodeReading {
    uint8_t     metricId;   ///< nodecfg MetricId (DATA2 `metric`)
    uint8_t     index;      ///< DATA2 `index` (probe ordinal), else 0
    float       value;
    char        name[nodecfg::METRIC_NAME_CAP];   ///< the ingest metric name
    const char* unit;       ///< static string from the catalogue
};

/// Every metric a valid config can produce, battery excluded (the ESP-NOW
/// node adds battery_voltage itself; it is not a sensor-list entry).
static const int NODE_MAX_READINGS = nodecfg::MAX_METRICS;

/// Bring up exactly the sensors in `cfg.sensors`, on `cfg.i2c` and each
/// entry's pins, tearing down whatever a previous call set up (so a config
/// applied live takes effect without a restart). `cfg` must already have
/// passed nodecfg::validate(). Returns how many entries answered. Safe to
/// call again with the same config: an entry that failed cold is retried.
int nodeSensorsBegin(const nodecfg::NodeConfig& cfg);

/// True once at least one configured entry answered.
bool nodeSensorsReady();

/// Read one value of every metric the config publishes, in listMetrics()
/// order, skipping metrics whose sensor is not answering and pressure_sea
/// when altitude_m == 0. Returns how many were written (<= maxOut).
int nodeSensorsRead(const nodecfg::NodeConfig& cfg, NodeReading* out, int maxOut);

/// Short human description for the boot log and /api/status,
/// e.g. "bmx280@0x76 ok, ds18b20x2@GPIO12 ok, pulse@GPIO4 rain".
const char* nodeSensorsDescribe();

/// Release interrupts and serial ports before deep sleep or a restart.
void nodeSensorsEnd();
