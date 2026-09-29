#include "UsbCdcConfigModule.h"
#include "ModuleSchemas.h"      // this module's form, gzipped
#include "UsbCdcModule.h"   // global `usbCdc`

// ---------------------------------------------------------------------------
bool UsbCdcConfigModule::load(JsonObjectConst cfg) {
    if (!usbCdc.isUsbCdcSupported()) return true;     // nothing to apply
    if (cfg["onBoot"].is<bool>()) usbCdc.setUsbCdcEnabled(cfg["onBoot"].as<bool>());
    return true;
}

// ---------------------------------------------------------------------------
bool UsbCdcConfigModule::save(JsonObject cfg) const {
    cfg["onBoot"]            = usbCdc.isUsbCdcEnabled();          // saved preference
    cfg["supported"]         = usbCdc.isUsbCdcSupported();
    cfg["board"]             = usbCdc.getBoardName();
    cfg["pins"]              = usbCdc.getUsbPins();
    cfg["activeNow"]         = usbCdc.isUsbCdcActiveAtBoot();     // live build-flag state
    cfg["recompileRequired"] = (usbCdc.isUsbCdcEnabled() != usbCdc.isUsbCdcActiveAtBoot());
    return true;
}

// ---------------------------------------------------------------------------
// A single on/off toggle (src/modules/schemas/usbcdc.json). The form only
// renders on boards that support USB CDC; elsewhere there is no schema and
// the module is status-only.
ModuleSchema UsbCdcConfigModule::schema() const {
    if (!usbCdc.isUsbCdcSupported()) return {nullptr, 0};
    return {MODULE_SCHEMA_USBCDC_GZ, sizeof(MODULE_SCHEMA_USBCDC_GZ)};
}

// ---------------------------------------------------------------------------
// Live status chip — reports the actual (build-flag) state and flags a pending
// recompile when the saved preference differs from what is running now.
void UsbCdcConfigModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;                       // UI shows "disabled"
    if (!usbCdc.isUsbCdcSupported()) {
        out["text"] = "not supported on this board";
        out["tone"] = "dim";
        return;
    }
    bool active = usbCdc.isUsbCdcActiveAtBoot();
    String t = active ? (String("on \xC2\xB7 GPIO ") + usbCdc.getUsbPins() + " locked")
                      : "off \xC2\xB7 GPIO free";
    if (usbCdc.isUsbCdcEnabled() != active) {
        t += " \xC2\xB7 recompile pending";
        out["tone"] = "warn";
    } else {
        out["tone"] = "ok";
    }
    out["text"] = t;
}
