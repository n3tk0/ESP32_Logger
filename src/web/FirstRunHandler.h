// ============================================================================
// src/web/FirstRunHandler.h
//
// R11 first-run wizard backend.
//
// Two endpoints, both intentionally unauthenticated — they must be
// reachable on a fresh device before the user has configured anything.
// The FirstRunGate in WebServer.cpp restricts them (and the wizard
// HTML) to be the only routes accessible while g_setupRequired is true.
//
//   GET  /api/board-profiles     — JSON list of profiles + pin rules
//   POST /api/firstrun           — save profile + (legacy) pin selections,
//                                  then schedule a reboot into normal mode
//
// Register both in setupWebServer() AFTER FirstRunGate is installed.
// ============================================================================
#pragma once

#include <ESPAsyncWebServer.h>

void registerFirstRunRoutes();
