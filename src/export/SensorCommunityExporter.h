#pragma once
#include "IExporter.h"
// <HTTPClient.h> moved to SensorCommunityExporter.cpp — not referenced in this header.

// ============================================================================
// SensorCommunityExporter — sends data to https://api.sensor.community
// Implements the Airrohr / sensors-software upload protocol.
//
// Config keys: "enabled", "interval_ms" (145000 = ~2.4 min, required by API)
// The device ID must match a registered sensor on sensor.community.
//
// Supports sensors:
//   X-Pin 1  → SDS011 / PMS5003  (pm25=P2, pm10=P1)
//   X-Pin 11 → BME280            (temperature, humidity, pressure)
// ============================================================================
class SensorCommunityExporter : public IExporter {
public:
    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "sensor_community"; }
    bool        isEnabled() const override { return _enabled; }
    // API requirement — ExportManager clamps the configured interval up to it.
    uint32_t    minIntervalMs()  const override { return 145000; }
    // The upload protocol has no timestamp field: a replayed spool would put
    // old values at "now", so failed batches are dropped, not spooled.
    bool        spoolOnFailure() const override { return false; }

private:
    bool _postPin(const char* pin, const char* sensorName,
                  const char* body);

    char     _deviceId[13]  = {};

    static constexpr const char* API_URL =
        "https://api.sensor.community/v1/push-sensor-data/";
};
