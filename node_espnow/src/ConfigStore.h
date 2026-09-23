// ============================================================================
// node_espnow/src/ConfigStore.h
//
// The node's settings in NVS — docs/NODE_CONFIG.md §2:
//
//   namespace "cfg", key "doc"   the §1 document as compact JSON, without
//                                secrets (an ESP-NOW node has none but the key)
//   namespace "cfg", key "lmk"   the 16-byte key typed on the node's own page;
//                                absent = the compiled ESPNOW_LMK
//
// The link state (node id, channel, collector MAC, BSSID, SSID) stays in the
// "espnow-node" namespace where it has always been; main.cpp owns that.
//
// Everything is written only when something changed — a config from the
// collector, a save on the page, the collector's legacy interval push. Never
// on an ordinary wake: this node's whole budget is not spending energy, and
// NVS is flash.
// ============================================================================
#pragma once

#include <stdint.h>

#include "src/nodecfg/NodeConfig.h"
#include "src/nodecfg/NodeConfigValidate.h"

/// The config this firmware starts from when nothing is stored: every value
/// from node_config.h, and the one BME280 the node has always had.
void cfgStoreDefaults(nodecfg::NodeConfig& out);

/// Defaults, overlaid with the stored document (when it decodes AND
/// validates — a stored config the current validator refuses is ignored with
/// a log line rather than run), plus the stored key.
void cfgStoreLoad(nodecfg::NodeConfig& out);

/// Write `c` as the stored document. False when it did not fit or NVS failed.
bool cfgStoreSave(const nodecfg::NodeConfig& c);

/// Store the page's key ("" removes it, returning to the compiled one).
bool cfgStoreSaveKey(const char* lmk);

/// The 16 bytes the link uses: the page's key when set, else ESPNOW_LMK.
const char* cfgActiveKey(const nodecfg::NodeConfig& c);

/// True when the active key is the placeholder this repository ships —
/// i.e. nobody has chosen one, at build time or on the page.
bool cfgKeyIsPlaceholder(const nodecfg::NodeConfig& c);
