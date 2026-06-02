#pragma once
#include "../core/IModule.h"

// ============================================================================
// UsbCdcConfigModule — IModule adapter exposing the USB-CDC-on-boot preference
// (owned by the global `usbCdc`) in the schema-driven module manager.
//
// The on-boot mode is ultimately a compile-time build flag
// (ARDUINO_USB_CDC_ON_BOOT), so saving here persists the preference to NVS and
// it takes effect on the next firmware recompile/flash. statusJson() reports
// the live build-flag state plus whether a recompile is pending. Kept as a
// SEPARATE adapter so UsbCdcModule itself stays free of the IModule/ArduinoJson
// dependency (it is compiled into the host-test/fuzz builds).
// ============================================================================
class UsbCdcConfigModule : public IModule {
public:
    const char* getId()   const override { return "usbcdc"; }
    const char* getName() const override { return "USB CDC"; }
    const char* getDescription() const override {
        return "USB serial-on-boot. Off frees the USB GPIO pins (applied on next flash).";
    }
    void statusJson(JsonObject out) const override;

    bool load(JsonObjectConst cfg) override;
    bool save(JsonObject cfg)      const override;
    const char* schema() const override;

    static UsbCdcConfigModule& instance() { static UsbCdcConfigModule m; return m; }
};
