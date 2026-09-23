// ============================================================================
// node/src/NodeCfgTables.h — the two src/nodecfg calls that pull in its
// tables, routed through one translation unit.
//
// src/nodecfg/HwPins.h and MetricCatalog.h define their pin, board and metric
// tables as `static const` in the header, which is right for the host tests
// and the collector — and on this firmware means one private copy per .cpp
// that calls a function reading them. On the ESP8266 const data is RAM
// (.rodata sits in DRAM), and a copy is ~1.5 KB with its strings. validate()
// and encodeCaps() read them, and so does the sensor layer; so both are
// defined in node_sensors_impl.cpp, next to it, and everything else in node/
// calls them through here. One copy instead of three.
//
// Anything else from src/nodecfg (encode/decode, the migration, UdpDiscovery)
// reads no table and can be called from anywhere.
// ============================================================================
#pragma once

#include <ArduinoJson.h>

#include "src/nodecfg/NodeConfig.h"
#include "src/nodecfg/NodeConfigValidate.h"

/// nodecfg::validate().
bool nodeValidate(const nodecfg::NodeConfig& c, nodecfg::Validation& v);

/// nodecfg::encodeCaps() for this node: WiFi transport, ESP8266.
void nodeEncodeCaps(JsonObject out);
