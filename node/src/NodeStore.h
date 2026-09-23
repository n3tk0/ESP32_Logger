// ============================================================================
// node/src/NodeStore.h
//
// Where the WiFi node keeps its settings: /config.json on LittleFS, the
// docs/NODE_CONFIG.md §1 document WITH secrets (§2), plus the previous copy
// in /config.prev.json that a rollback (§4.7) restores.
//
// The values in node_config.h — and the NODE_SENSOR_* selection — are not
// the configuration. They are the DEFAULTS that seed it on a blank
// filesystem, so `pio run -t upload` with build flags still produces a node
// that works on first boot, and every later change comes from the node's own
// page or from the collector.
//
// WHY LITTLEFS AND NOT EEPROM
// ---------------------------
// The ESP8266 core's EEPROM emulation is a RAM copy of one flash sector,
// rewritten whole on every commit. A partial write during a brownout leaves
// the sector in whatever state it reached. LittleFS does the copy-on-write
// and metadata work already, and every save here writes a temp file and
// renames it — so a power loss mid-save leaves the previous config intact
// rather than a half-written one.
// ============================================================================
#pragma once

#include <Arduino.h>

#include "src/nodecfg/NodeConfig.h"
#include "NodeSync.h"

/// Mount LittleFS (formatting it if it has never been used) and load the
/// config into `out`:
///
///   1. the defaults: nodecfg::configDefaults() for this chip, then
///      node_config.h and the NODE_SENSOR_* list on top;
///   2. /config.json on top of those — the §1 document, or the old flat
///      format (ssid, intervalMs, i2cSda, …), which is migrated and saved
///      back in the new one;
///   3. if /config.json is unreadable, /config.prev.json instead.
///
/// transport, hw and fw are always this firmware's, whatever the file says.
/// Returns false when no config file could be read (the defaults are in
/// `out` then).
bool storeLoad(nodecfg::NodeConfig& out);

/// Write `c` (with secrets) to /config.json via a temp file + rename. With
/// `backup`, the file being replaced is first copied to /config.prev.json
/// (also temp + rename) — skipped while a network change is on trial, so the
/// backup keeps holding the config a rollback must return to.
bool storeSave(const nodecfg::NodeConfig& c, bool backup);

/// §4.7: put /config.prev.json back as /config.json. False when there is no
/// backup or the copy failed; /config.json is untouched then.
bool storeRollback();

/// True when the config has the minimum for the node to have anything to
/// do: a network name and a collector address. Anything less and the portal
/// has to run, because there is no fallback that could work.
bool storeIsComplete(const nodecfg::NodeConfig& c);

/// The node's side of the exchange that must survive a restart: the rev of
/// a network change on trial (§4.7, 0 = none), and a cfg_error not yet
/// delivered — a rollback restarts the node, and the collector still has to
/// hear about it. Kept in /sync.json, apart from the §1 document, which has
/// no field for either.
struct SyncState {
    uint16_t           trialRev = 0;
    NodeSync::CfgError err;
};

bool syncLoad(SyncState& out);
bool syncSave(const SyncState& s);
