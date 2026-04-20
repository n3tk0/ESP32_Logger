#pragma once
#include "../core/IModule.h"

// ============================================================================
// OtaModule — IModule adapter over OtaManager (Pass 5, phase 2).
//
// OTA has almost no persistent configuration in DeviceConfig — rollback state
// lives in the ESP-IDF otadata partition, confirmation is time-based.  This
// adapter is therefore mostly informational: it exposes the running/previous
// partition labels and rollback capability as read-only config fields.
//
// hasUI() returns false for now (schema is nullptr); the registry lists the
// module in the tab strip but the UI only shows an enable/disable switch +
// the informational block that already lives at /update.
// ============================================================================
class OtaModule : public IModule {
public:
    const char* getId()   const override { return "ota"; }
    const char* getName() const override { return "OTA update"; }

    bool load(JsonObjectConst cfg) override;
    void save(JsonObject cfg)      const override;

    // Informational module — no form for phase 2.
    const char* schema() const override { return nullptr; }

    static OtaModule& instance() { static OtaModule m; return m; }
};
