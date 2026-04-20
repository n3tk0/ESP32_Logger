#pragma once
#include "../core/IModule.h"

// ============================================================================
// ThemeModule — IModule adapter over config.theme (Pass 5, phase 2).
//
// Mirrors the theme section of DeviceConfig as JSON so the new /api/modules
// endpoints (phase 3) and schema-driven UI (phase 4) can work against a
// common shape.  Phase 2 only ships the adapter; modules.json is kept in
// sync via saveConfig(), and /config.bin remains authoritative.
// ============================================================================
class ThemeModule : public IModule {
public:
    const char* getId()   const override { return "theme"; }
    const char* getName() const override { return "Theme"; }

    bool load(JsonObjectConst cfg) override;
    void save(JsonObject cfg)      const override;

    const char* schema() const override;

    static ThemeModule& instance() { static ThemeModule m; return m; }
};
