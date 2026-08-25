// ============================================================================
// src/web/IngestHandler.h
//
// POST /api/ingest — the door remote sensor nodes push readings through.
//
// Compiled in only when FEATURE_REMOTE_NODES is defined (see setup.h); the
// registration call and every byte behind it drop out otherwise, which
// matters on the 4 MB C3 targets.
//
// Request body:
//   {
//     "node": "balcony",
//     "ts":   1723800000,          // optional; 0/absent = collector stamps
//     "readings": [
//       { "metric": "temperature", "value": 12.34, "unit": "C"   },
//       { "metric": "pressure",    "value": 1013.2, "unit": "hPa" },
//       { "metric": "humidity",    "value": 61.5,  "unit": "%"   }
//     ]
//   }
//
// Response: {"ok":true,"stored":3,"rejected":0}
//
// AUTHENTICATION
// --------------
// This endpoint deliberately does NOT go through requireMutatingAuth(),
// because that chain includes a CSRF token check. CSRF exists to stop a
// third-party page from making a *browser* issue a state-changing request
// on the strength of ambient credentials it carries automatically. A sensor
// node has no cookie jar, no session and no origin — there is nothing for
// CSRF to protect, and no way for the node to obtain a token.
//
// What it does instead:
//   • the same rate limiter every mutating endpoint uses;
//   • a shared token (INGEST_TOKEN) compared in constant time, sent as
//     `X-Ingest-Token` or `?token=`;
//   • whatever HTTP Basic Auth is compiled in globally, which the front-door
//     handler applies before this handler is ever reached.
//
// The token is a compile-time constant rather than a config field on
// purpose: it keeps the flash cost near zero, and a device that can be
// reconfigured over the same LAN gains little from making it editable there.
// Change it with -DINGEST_TOKEN='"..."' per deployment.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

class AsyncWebServer;

/// Registers POST /api/ingest on `server`. Call from setupWebServer().
void registerIngestHandler(AsyncWebServer& server);

#endif  // FEATURE_REMOTE_NODES
