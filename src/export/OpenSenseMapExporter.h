#pragma once
#include "IExporter.h"
// <HTTPClient.h> moved to OpenSenseMapExporter.cpp — not referenced in this header.

// openSenseMap over plain HTTP on the C3, the same reason as Open-Meteo in
// ForecastModule.cpp: a TLS session wants two 16 KB mbedTLS record buffers on
// top of the handshake, and a C3 with an SD card mounted does not have that
// much contiguous heap — the upload failed with an allocation error.
// ingress.opensensemap.org is the endpoint openSenseMap provides for devices
// without TLS (api.opensensemap.org only redirects plain HTTP to HTTPS).
// The cost: the box's access token crosses the LAN/ISP in clear text. It only
// authorises posting measurements to this one box, and setInsecure() never
// verified the server anyway, but where the heap allows it (S3), stay on TLS.
// Override with -DOSM_TLS=1 (or 0) to choose on any chip.
#ifndef OSM_TLS
#  if defined(CONFIG_IDF_TARGET_ESP32C3)
#    define OSM_TLS 0
#  else
#    define OSM_TLS 1
#  endif
#endif
#if OSM_TLS
#  define OSM_API_BASE "https://api.opensensemap.org/boxes/"
#else
#  define OSM_API_BASE "http://ingress.opensensemap.org/boxes/"
#endif

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
    const char* _sendableId(const SensorReading& r) const;
};
