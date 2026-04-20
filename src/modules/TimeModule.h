#pragma once
#include "../core/IModule.h"

// ============================================================================
// TimeModule — IModule adapter over the time-related fields of DeviceConfig
// (ntpServer, timezone, dstOffsetHours).  Pass 5, phase 2.
//
// These live under config.network for historical reasons, but logically
// belong to their own module so the Settings UI can render a dedicated
// "Time" tab alongside WiFi/OTA/Theme/DataLog.
// ============================================================================
class TimeModule : public IModule {
public:
    const char* getId()   const override { return "time"; }
    const char* getName() const override { return "Time"; }

    bool load(JsonObjectConst cfg) override;
    void save(JsonObject cfg)      const override;

    const char* schema() const override;

    static TimeModule& instance() { static TimeModule m; return m; }
};
