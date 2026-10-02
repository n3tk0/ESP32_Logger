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
#include <math.h>

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
/// order, skipping metrics whose sensor is not answering, pressure_sea when
/// altitude_m == 0, and every entry whose own interval_s has not come round
/// on send number `tick` (nodecfg::sensorDue(); tick 0 reads everything).
/// An SDS011 that is due but has not finished its warm-up is skipped and
/// read on a later send instead. Returns how many were written (<= maxOut).
int nodeSensorsRead(const nodecfg::NodeConfig& cfg, NodeReading* out, int maxOut,
                    uint32_t tick);

/// Between sends, on a node that stays awake: call often (every loop pass).
/// `nextTick` is the number the next send will have and `msToNext` how long
/// until it. Wakes a sleeping SDS011 its warm-up ahead of the send that reads
/// it, and takes its frames as they arrive. Does nothing without an SDS011.
void nodeSensorsIdle(const nodecfg::NodeConfig& cfg, uint32_t nextTick, uint32_t msToNext);

/// How the sensor layer waits for a sensor to finish measuring: a DS18B20
/// conversion (up to 760 ms at 12 bits) and a one-shot BH1750 measurement
/// (180 ms), started together in nodeSensorsRead() and waited out once, and
/// a continuous BH1750's first measurement in nodeSensorsBegin(). nullptr,
/// the default, is delay() — right for the WiFi node, whose
/// radio stays associated, and for any node with an interrupt or a serial
/// sensor running. The ESP-NOW node in battery mode passes a light sleep
/// instead: the sensors measure on their own supply, so the CPU need not be
/// awake for it (docs/ESPNOW_NODE.md §9, "A DS18B20", "A BH1750").
///
/// The hook is called with the whole conversion time and must not return
/// before at least that much has passed.
void nodeSensorsSetWait(void (*wait)(uint32_t ms));

/// Short human description for the boot log and /api/status,
/// e.g. "bmx280@0x76 ok, ds18b20x2@GPIO12 ok, pulse@GPIO4 rain".
const char* nodeSensorsDescribe();

/// One configured entry, for a diagnostics page.
struct NodeSensorDiag {
    const char* type;       ///< sensorTypeName()
    bool        ok;         ///< answered at its last bring-up
    uint8_t     addr;       ///< I2C address that answered
    uint8_t     found;      ///< ds18b20: probes on the bus
    uint32_t    reads;      ///< reads that gave at least one value
    uint32_t    empty;      ///< reads that gave none
    uint32_t    lastOkMs;   ///< millis() of the last good read, 0 = never
};

/// Fill `out` with the configured entries, in config order; returns how many.
int nodeSensorsDiag(NodeSensorDiag* out, int maxOut);

/// The SDS011's serial line, for telling a wiring fault (no bytes), a wrong
/// baud rate or noise (bytes, no frames) and a sensor left in periodic or
/// query mode (frames rare or none, `period`/`reportMode` non-zero) apart.
struct NodeSdsDiag {
    bool     up = false, awake = false, warmed = false, pending = false;
    uint32_t bytes   = 0;   ///< every byte received
    uint32_t frames  = 0;   ///< valid measurement frames
    uint32_t badSum  = 0;   ///< measurement frames with a bad checksum
    uint32_t other   = 0;   ///< command replies and misaligned frames
    uint32_t used    = 0;   ///< frames that went out as a reading
    uint32_t giveUps = 0;   ///< due reads abandoned for want of a frame
    uint32_t wokeMs  = 0;   ///< millis() of the last wake command
    uint32_t frameMs = 0;   ///< millis() of the newest frame, 0 = none
    int16_t  reportMode = -2;   ///< last query: 0 active, 1 query, -1 no answer, -2 not asked
    int16_t  period     = -2;   ///< last query: minutes, 0 continuous, -1/-2 as above
    float    pm25 = NAN, pm10 = NAN;   ///< the last values sent
};
const NodeSdsDiag& nodeSensorsSdsDiag();

/// Release interrupts and serial ports before deep sleep or a restart.
void nodeSensorsEnd();
