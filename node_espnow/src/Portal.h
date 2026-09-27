// ============================================================================
// node_espnow/src/Portal.h
//
// The node's own setup page — docs/NODE_CONFIG.md §6 — on a WPA2 access point
// named esp-node-XXXX (password PORTAL_AP_PASS). The page is the shared one in
// src/nodecfg/NodePortalPage.h; this file serves it and its JSON API:
//
//   GET  /             the page, gzip
//   GET  /api/config   {"config": §1 without secrets, with lmk_set, "caps": …}
//   POST /api/config   §1 (partial allowed) → validate → save, local = true,
//                      restart; or 400 {"ok":false,"field","reason"}
//   GET  /api/scan     {"state":"running"} / {"state":"done","nets":[…]}
//   GET  /api/status   uptime_s, mac, ip, rev, local, paired, node_id, ch,
//                      batt_v, collector ("unknown": the radio is off)
//   POST /update       multipart, field `fw`: a node_espnow firmware, checked
//                      and written to the other OTA slot, then restart —
//                      docs/NODE_OTA.md §5
//
// WHEN IT RUNS
// ------------
// * BOOT (GPIO9) held — see portalRequested() for why "held through reset"
//   cannot literally be what the user does on a C3.
// * On a power-on, when the node has never been paired AND has no key but the
//   placeholder: a node in that state cannot do anything useful, and a page is
//   the only way to give it a key without a rebuild.
//
// ESP-NOW is not started while the portal runs (it is called before
// linkBegin()), and the portal always ends in a restart: on save, 500 ms after
// the reply; otherwise after PORTAL_TIMEOUT_MS with no station connected.
// ============================================================================
#pragma once

#include <stdint.h>

#include "Link.h"
#include "src/nodecfg/NodeConfig.h"

/// True when the user is asking for the portal. `coldStart` = this boot is a
/// power-on or the reset button (not a deep-sleep wake or a software restart),
/// which is when it is worth watching the button for a moment.
bool portalRequested(bool coldStart);

/// True when the button is held right now (debounced). Cheap; the mains loop
/// polls it between reports.
bool portalButtonHeld();

/// Run the portal. Never returns: it restarts the chip. `battV` reads the
/// cell (volts, 0 = unknown) for /api/status — a function rather than a value
/// so the page's trim helper sees a fresh reading on every poll.
[[noreturn]] void portalRun(nodecfg::NodeConfig& cfg, const NodeLink& link, float (*battV)());
