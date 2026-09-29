#include "OtaModule.h"
#include "ModuleSchemas.h"      // this module's form, gzipped
#include "../managers/OtaManager.h"

// ---------------------------------------------------------------------------
bool OtaModule::load(JsonObjectConst cfg) {
    int sec = cfg["autoConfirmSec"] | (int)_autoConfirmSec;
    if (sec < 10)   sec = 10;
    if (sec > 3600) sec = 3600;
    _autoConfirmSec       = (uint16_t)sec;
    _requireManualConfirm = cfg["requireManualConfirm"] | _requireManualConfirm;

    // Push the policy into OtaManager. load() runs from moduleRegistry.loadAll()
    // which precedes OtaManager::boot() in setup(), so this applies to the
    // current boot's pending image too.
    OtaManager::setConfirmPolicy((uint32_t)_autoConfirmSec * 1000UL, _requireManualConfirm);
    return true;
}

// ---------------------------------------------------------------------------
bool OtaModule::save(JsonObject cfg) const {
    // Editable settings (round-tripped through modules.json).
    cfg["autoConfirmSec"]       = _autoConfirmSec;
    cfg["requireManualConfirm"] = _requireManualConfirm;
    // Read-only status surfaced to /api/modules/ota for the actions panel.
    cfg["running"]              = OtaManager::runningPartitionLabel();
    cfg["previous"]             = OtaManager::previousPartitionLabel();
    cfg["pendingVerify"]        = OtaManager::isPendingVerify();
    cfg["rollbackCapable"]      = OtaManager::isRollbackCapable();
    cfg["confirmInMs"]          = OtaManager::millisUntilConfirm();
    return true;
}

// ---------------------------------------------------------------------------
// The editable settings (src/modules/schemas/ota.json). The read-only
// partition/rollback info travels in the `config` block (save() above) and is
// rendered by the manager's OTA status+actions panel rather than as form
// inputs.
ModuleSchema OtaModule::schema() const {
    return {MODULE_SCHEMA_OTA_GZ, sizeof(MODULE_SCHEMA_OTA_GZ)};
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
