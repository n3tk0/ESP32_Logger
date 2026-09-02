// ============================================================================
// node/src/ConfigPortal.h
//
// A temporary access point with a one-page settings form, for reconfiguring
// a node that is already mounted somewhere inconvenient.
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
//                               go back to trying the configured network.
//                               Repeat. The node self-heals when the router
//                               comes back, and is still reachable in the
//                               window if the credentials genuinely changed.
//   • FLASH button held at boot → run until configured, same as no config.
//                               This is the deliberate "let me in" path.
//
// SECURITY POSTURE
// ----------------
// The AP is WPA2 with a passphrase, not open. It only exists while the node
// cannot reach its network, but an open AP in that window would let anyone in
// range repoint the node at their own collector, or read the WiFi passphrase
// back out of the form. Change PORTAL_AP_PASS from the default.
//
// In normal operation the node runs no server and listens on no port; the
// portal's HTTP stack only exists while the AP is up.
// ============================================================================
#pragma once

#include "NodeSettings.h"

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

/// True when the FLASH button is held at boot. Call early in setup(), before
/// anything else claims the pin.
bool portalButtonHeld();

/// Brings up the AP and serves the settings form.
///
/// `timeoutMs` of 0 means run until the user saves (used when there is no
/// usable config). Otherwise the portal closes after that long and returns,
/// so the caller can retry the configured network.
///
/// Returns true if settings were saved — the caller should restart. Returns
/// false on timeout, with `s` unmodified.
bool portalRun(NodeSettings& s, uint32_t timeoutMs);

/// Serve the configuration form on the STA interface — the home LAN — for as
/// long as the node is up, so it can be reconfigured without walking to it.
///
/// GATED ON THE BASIC-AUTH CREDENTIALS, and returns false without starting
/// anything when they are unset. This form rewrites the collector address and
/// restarts the node; on the LAN it is reachable by everything on the network,
/// unlike the access-point portal, which needs the AP's own password and only
/// exists in a short window. The passphrase and the ingest token are also
/// blanked in this mode rather than rendered into the page.
bool portalStartBackground(NodeSettings& s);

/// Services background HTTP requests. Restarts the ESP if settings are saved.
void portalHandleClient();
