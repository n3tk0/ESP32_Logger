#pragma once
#include "../core/IModule.h"

// ============================================================================
// OtaModule — IModule adapter over OtaManager (Pass 5).
//
// Persisted state lives in modules.json (NOT config.bin — its binary layout is
// version-locked): the auto-confirm window and the "require manual confirm"
// flag, fed into OtaManager::setConfirmPolicy() at load(). The read-only
// partition/rollback info is also serialised so /api/modules/ota carries it,
// and the manager UI renders a status + actions panel (Confirm / Rollback) on
// top of the schema form.
// ============================================================================
class OtaModule : public IModule {
public:
    const char* getId()   const override { return "ota"; }
    const char* getName() const override { return "OTA update"; }
    const char* getDescription() const override {
        return "Firmware updates and A/B rollback (running/previous partition).";
    }
    void statusJson(JsonObject out) const override;

    bool load(JsonObjectConst cfg) override;
    bool save(JsonObject cfg)      const override;

    const char* schema() const override;

    static OtaModule& instance() { static OtaModule m; return m; }

private:
    // Defaults mirror OtaManager's historical compile-time behavior.
    uint16_t _autoConfirmSec       = 90;     // OTA_CONFIRM_TIMEOUT_MS / 1000
    bool     _requireManualConfirm = false;
};
