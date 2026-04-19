#pragma once
#include "../core/IModule.h"

// ============================================================================
// DataLogModule — IModule adapter over config.datalog (Pass 5, phase 2).
//
// Mirrors the datalog section of DeviceConfig as JSON for /api/modules/:id.
// /config.bin stays authoritative; saveConfig() keeps modules.json in sync.
// ============================================================================
class DataLogModule : public IModule {
public:
    const char* getId()   const override { return "datalog"; }
    const char* getName() const override { return "Data log"; }

    bool load(JsonObjectConst cfg) override;
    void save(JsonObject cfg)      const override;

    const char* schema() const override;

    static DataLogModule& instance() { static DataLogModule m; return m; }
};
