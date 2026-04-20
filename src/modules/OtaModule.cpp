#include "OtaModule.h"
#include "../managers/OtaManager.h"

// ---------------------------------------------------------------------------
bool OtaModule::load(JsonObjectConst /*cfg*/) {
    // No persisted fields yet — phase 2 is informational only.
    return true;
}

// ---------------------------------------------------------------------------
void OtaModule::save(JsonObject cfg) const {
    // Read-only status.  Serialised into modules.json every saveConfig() so
    // /api/modules/ota returns a useful payload once phase-3 endpoints ship.
    cfg["running"]          = OtaManager::runningPartitionLabel();
    cfg["previous"]         = OtaManager::previousPartitionLabel();
    cfg["pendingVerify"]    = OtaManager::isPendingVerify();
    cfg["rollbackCapable"]  = OtaManager::isRollbackCapable();
}
