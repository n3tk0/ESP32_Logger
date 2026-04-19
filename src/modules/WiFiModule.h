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

    const char* schema() const override;

    static WiFiModule& instance() { static WiFiModule m; return m; }
};
