// ============================================================================
// node/src/NodeSettings.h
//
// Runtime settings, stored as JSON in LittleFS at /config.json.
//
// The values in node_config.h are no longer the configuration — they are the
// DEFAULTS that seed it on a blank filesystem. That keeps `pio run -t upload`
// with build flags working exactly as before for anyone who prefers it, while
// letting a node on a wall be reconfigured without a cable.
//
// WHY LITTLEFS AND NOT EEPROM
// ---------------------------
// The ESP8266 core's EEPROM emulation is a RAM copy of one flash sector,
// rewritten whole on every commit. A partial write during a brownout leaves
// the sector in whatever state it reached. LittleFS does the copy-on-write
// and metadata work already, and save() writes to a temp file and renames —
// so a power loss mid-save leaves the previous config intact rather than a
// half-written one.
// ============================================================================
#pragma once

#include <Arduino.h>

struct NodeSettings {
    // Sizes are the protocol/standard maxima, not guesses: an SSID is at most
    // 32 bytes, a WPA2 passphrase at most 64, and the collector clamps node
    // ids to 16 (SensorReading::sensorId).
    char     ssid[33]      = {0};
    char     pass[65]      = {0};
    char     host[40]      = {0};
    uint16_t port          = 80;
    char     token[41]     = {0};
    char     nodeId[17]    = {0};
    char     basicUser[33] = {0};
    char     basicPass[33] = {0};
    uint32_t intervalMs    = 60000;
    float    altitudeM     = 0.0f;

    // Which board this firmware is running on. It changes NOTHING the node
    // does — the ESP8266's pins are the same on all of them — and everything
    // the setup portal SHOWS: the header diagram, and which silkscreen label
    // belongs to which GPIO. Kept here rather than compiled in because the
    // same binary is flashed to whatever is on the shelf.
    //   0 = NodeMCU V2/V3, 1 = Wemos D1 mini, 2 = bare ESP-12 / other
    uint8_t  board         = 0;

    // Sensor wiring. Defaults come from node_config.h; the portal overrides
    // them per device, and only shows the fields whose sensor is actually
    // compiled into this build.
    uint8_t  i2cSda        = 4;
    uint8_t  i2cScl        = 5;
    uint8_t  oneWirePin    = 12;
    uint8_t  pulsePin      = 13;
    uint8_t  sdsRx         = 14;
    uint8_t  sdsTx         = 13;

    /// True once an SSID and a collector host are both present — the minimum
    /// for the node to have anything to do. Anything less and the portal has
    /// to run, because there is no fallback that could work.
    bool isComplete() const {
        return ssid[0] != '\0' && host[0] != '\0';
    }
};

/// Mounts LittleFS (formatting it if it has never been used) and loads
/// /config.json into `out`. Fields missing from the file keep the compiled-in
/// defaults, so adding a setting in a later firmware does not invalidate a
/// config written by an earlier one. Returns false when no config file exists.
bool settingsLoad(NodeSettings& out);

/// Writes `s` to /config.json via a temp file + rename, so an interrupted
/// save cannot destroy a working config. Returns false on any FS error.
bool settingsSave(const NodeSettings& s);

/// Deletes /config.json. The next boot comes up in the portal.
bool settingsErase();
