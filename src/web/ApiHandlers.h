#pragma once
#include <ESPAsyncWebServer.h>

// ============================================================================
// ApiHandlers — registers new /api/data and /api/sensors routes.
// Call registerApiRoutes() from setupWebServer() or Logger.ino.
// ============================================================================
void registerApiRoutes(AsyncWebServer& server);
