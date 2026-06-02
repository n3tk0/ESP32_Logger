#include "OtaModule.h"
#include "../managers/OtaManager.h"

// ---------------------------------------------------------------------------
bool OtaModule::load(JsonObjectConst /*cfg*/) {
    // No persisted fields yet — phase 2 is informational only.
    return true;
}

// ---------------------------------------------------------------------------
bool OtaModule::save(JsonObject cfg) const {
    // Read-only status.  Serialised into modules.json every saveConfig() so
    // /api/modules/ota returns a useful payload once phase-3 endpoints ship.
    cfg["running"]          = OtaManager::runningPartitionLabel();
    cfg["previous"]         = OtaManager::previousPartitionLabel();
    cfg["pendingVerify"]    = OtaManager::isPendingVerify();
    cfg["rollbackCapable"]  = OtaManager::isRollbackCapable();
    return true;
}

// ---------------------------------------------------------------------------
// Live status chip — running partition + pending-verify flag.  OtaManager
// getters read cached partition info, so this is cheap.
void OtaModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;                       // UI shows "disabled"
    String t = OtaManager::runningPartitionLabel();
    if (OtaManager::isPendingVerify()) {
        t += " \xC2\xB7 pending verify";
        out["tone"] = "warn";
    } else {
        out["tone"] = "ok";
    }
    out["text"] = t;
}
