// ============================================================================
// src/web/SettingsJson.h — the settings file (GET /export_settings,
// POST /import_settings).
//
// The sections a module owns are written and read by that module's own
// save()/load() — the same code the Modules page and modules.json go through
// — so a field is added in one place and a backup cannot skip it, and an
// import gets the module's validation rather than a copy of it that drifted.
// What remains here is what no module owns (identity, flow meter, logger,
// hardware, the AP side of the network) and the Kindle section, which has its
// own shared table (KindleConfigJson.h).
//
// Pure configuration in, pure configuration out: no file system, no web
// server. The handlers in WebServer.cpp top up defaults before exporting and
// save, re-apply the time zone and restart the logger after importing.
// tests/host/test_settings_json.cpp checks that export → import → export
// gives back the same file.
// ============================================================================
#pragma once

#include <ArduinoJson.h>

// The whole file. The WiFi passwords are "***" unless revealSecrets.
void settingsToJson(JsonObject out, bool revealSecrets);

// Applies a settings file to `config`. Keys that are absent leave their field
// as it is, so a file from an older firmware restores what it carried.
void settingsFromJson(JsonObjectConst in);
