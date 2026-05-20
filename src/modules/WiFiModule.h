#pragma once
#include "../core/IModule.h"

// ============================================================================
// WiFiModule — IModule adapter over config.network (Pass 5, phase 2).
//
// Read/write side of the existing DeviceConfig.network section, serialised as
// JSON so it can live in /config/modules.json alongside other modules and
// (eventually) drive the new schema-based Settings UI.  No new persisted
// state; /config.bin remains authoritative for phase 2 and modules.json is
// a shadow kept in sync via saveConfig().
// ============================================================================
class WiFiModule : public IModule {
public:
    const char* getId()   const override { return "wifi"; }
    const char* getName() const override { return "Wi-Fi"; }

    bool load(JsonObjectConst cfg) override;
    void save(JsonObject cfg)      const override;

    // R20: WiFi cannot be hot-restarted from a web handler — bringing the
    // radio down would close the very TCP connection serving this request.
    // start() returns false so /api/modules/wifi/enable + /restart correctly
    // report restartRequired=true; the caller (UI or operator) must
    // explicitly reboot via POST /restart to apply the new config.
    bool start() override { return false; }

    const char* schema() const override;

    static WiFiModule& instance() { static WiFiModule m; return m; }
};
