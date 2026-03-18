#pragma once
#include "IExporter.h"
#include <HTTPClient.h>

// ============================================================================
// OpenSenseMapExporter — sends data to openSenseMap.org REST API.
//
// Config keys:
//   enabled, box_id, access_token,
//   sensor_ids: {"temperature":"ID1","humidity":"ID2","pm25":"ID3",...}
// ============================================================================
class OpenSenseMapExporter : public IExporter {
public:
    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "opensensemap"; }
    bool        isEnabled() const override { return _enabled; }

private:
    char _boxId[33]  = {};
    char _token[65]  = {};

    // Map metric names to openSenseMap sensor IDs (max 12 sensors)
    struct SensorIdEntry {
        char metric[16];
        char sensorId[25];
    };
    SensorIdEntry _sensorIds[12];
    int           _sensorIdCount = 0;

    const char* _lookupSensorId(const char* metric) const;

    static constexpr const char* API_BASE = "https://api.opensensemap.org/boxes/";
};
