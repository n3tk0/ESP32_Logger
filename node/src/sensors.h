// ============================================================================
// node/src/sensors.h
//
// The node's sensor set, chosen at build time in node_config.h.
//
// Everything sensor-specific lives behind this interface so main.cpp does not
// grow an #ifdef per driver, and so adding a fourth sensor is one self-
// contained edit in sensors.cpp rather than a change threaded through the
// post loop, the portal and the settings struct.
//
// The drivers themselves are the collector's, included unmodified from
// ../src/drivers/. Metric names and units match the collector's own plugins
// exactly, so a remote reading and a wired one are the same series shape.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "NodeSettings.h"

// One measurement, ready to be serialised into an /api/ingest payload.
struct NodeReading {
    const char* metric;   // static string or owned by the sensor layer
    float       value;
    const char* unit;
};

// Eight DS18B20 probes plus four values from an I2C sensor is the realistic
// ceiling; the ingest table holds 8 metrics per node, so anything past that
// would be dropped on arrival anyway.
static constexpr int NODE_MAX_READINGS = 12;

/// Brings up every sensor compiled into this build, using the pins in `s`.
/// Returns the number that answered. Safe to call again later: a probe that
/// failed cold is retried, which is why main.cpp calls it from the post loop
/// as well as from setup().
int sensorsBegin(const NodeSettings& s);

/// True when at least one sensor is responding.
bool sensorsReady();

/// Samples everything and fills `out`, returning the count written.
/// Non-finite values are omitted rather than sent as nulls.
int sensorsRead(const NodeSettings& s, NodeReading* out, int maxOut);

/// Human-readable list of what is compiled in, e.g. "BME280, DS18B20(2)".
/// Printed at boot so the serial log says what this build can actually see.
const char* sensorsDescribe();
