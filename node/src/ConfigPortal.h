// ============================================================================
// node/src/ConfigPortal.h
//
// The node's own setup page (docs/NODE_CONFIG.md §6): the shared page from
// src/nodecfg/NodePortalPage.h, served gzip-compressed straight out of flash,
// and the JSON API it drives — GET/POST /api/config, GET /api/scan,
// GET /api/status. The same page, byte for byte, runs on the ESP-NOW node.
//
// WHEN IT RUNS — AND WHEN IT STOPS
// --------------------------------
// This is the part worth getting right. A portal that opens whenever WiFi is
// down and stays open would turn a router reboot at 3 am into a node that is
// still sitting in AP mode the next afternoon, having missed a night of
// readings while waiting for someone who was asleep.
//
// So the portal is time-boxed whenever there is a config worth retrying:
//
//   • No usable config saved  → run until configured. There is nothing else
//                               the node could be doing.
//   • Config saved, WiFi failed → run for PORTAL_TIMEOUT_MS, then close and
//                               go back to trying the configured network (or
//                               the collector's next one, §4). Repeat. The
//                               node self-heals when the router comes back,
//                               and is still reachable in the window if the
//                               credentials genuinely changed.
//   • FLASH button held at boot → run until configured, same as no config.
//                               This is the deliberate "let me in" path.
//
// The clock stops while a phone is associated with the AP, and restarts
// when it leaves.
//
// SECURITY POSTURE
// ----------------
// The AP is WPA2 with a passphrase, not open. It only exists while the node
// cannot reach its network, but an open AP in that window would let anyone in
// range repoint the node at their own collector. Change PORTAL_AP_PASS from
// the default.
//
// On the home LAN the same page and API are served for the node's whole
// uptime — but only behind HTTP basic auth, with the credentials the node
// already holds for the collector, and not at all when those are unset.
//
// Secrets are write-only everywhere (§0.5): GET /api/config never returns the
// WiFi passphrase, the ingest token or the basic-auth password, on the AP or
// on the LAN — only "<field>_set". A POST with "" keeps the stored one.
// ============================================================================
#pragma once

#include <stdint.h>

#include "src/nodecfg/NodeConfig.h"

// NodeMCU V3's FLASH button is on GPIO0, held LOW while pressed. Holding it
// through reset is the escape hatch when the saved credentials are wrong but
// the node keeps almost-connecting.
#ifndef PORTAL_TRIGGER_PIN
#  define PORTAL_TRIGGER_PIN 0
#endif

// WPA2 requires at least 8 characters.
#ifndef PORTAL_AP_PASS
#  define PORTAL_AP_PASS "configure"
#endif

// How long the portal stays up when there IS a config to fall back on.
#ifndef PORTAL_TIMEOUT_MS
#  define PORTAL_TIMEOUT_MS 300000UL   // 5 minutes
#endif

/// What /api/status says about the collector. Owned by main.cpp, which
/// updates it after every POST; the portal only reads it.
struct PortalLinkStatus {
    bool     attempted = false;   ///< any POST since boot
    bool     lastOk    = false;   ///< the latest POST was answered (HTTP 200)
    bool     everOk    = false;
    uint32_t lastOkMs  = 0;       ///< millis() of the latest answered POST
};

void portalSetLinkStatus(const PortalLinkStatus* s);

/// True when the FLASH button is held at boot. Call early in setup(), before
/// anything else claims the pin.
bool portalButtonHeld();

/// Brings up the AP and serves the page.
///
/// `timeoutMs` of 0 means run until the user saves (used when there is no
/// usable config). Otherwise the portal closes after that long without a
/// station associated and returns, so the caller can retry the network.
///
/// Returns true if settings were saved — the caller should restart. Returns
/// false on timeout, with `c` unmodified.
bool portalRun(nodecfg::NodeConfig& c, uint32_t timeoutMs);

/// Serve the page on the STA interface — the home LAN — for as long as the
/// node is up, so it can be reconfigured without walking to it.
///
/// GATED ON THE BASIC-AUTH CREDENTIALS (net.basic_user / net.basic_pass),
/// and returns false without starting anything when either is unset. The page
/// rewrites the collector address and restarts the node; on the LAN it is
/// reachable by everything on the network, unlike the access-point portal,
/// which needs the AP's own password and only exists in a short window.
bool portalStartBackground(nodecfg::NodeConfig& c);

/// Services background HTTP requests. Restarts the ESP if settings are saved.
void portalHandleClient();
