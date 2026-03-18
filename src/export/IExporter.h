#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "../core/SensorTypes.h"

// ============================================================================
// IExporter — abstract interface for all data exporters.
// Implementations: MqttExporter, HttpExporter,
//                  SensorCommunityExporter, OpenSenseMapExporter
// ============================================================================
class IExporter {
public:
    virtual ~IExporter() = default;

    // Called once with the exporter's config JSON object.
    // Example: {"enabled":true,"broker":"192.168.1.100","port":1883,...}
    virtual bool        init(JsonObjectConst config)                       = 0;

    // Send a batch of readings.  Returns true if all sent successfully.
    virtual bool        send(const SensorReading* readings, size_t count) = 0;

    // Short identifier string, e.g. "mqtt", "http", "sensor_community"
    virtual const char* getName()    const = 0;
    virtual bool        isEnabled()  const = 0;

    // Retry policy (used by ExportManager)
    virtual uint8_t     maxRetries()   const { return 3; }
    virtual uint32_t    retryDelayMs() const { return 5000; }

protected:
    bool _enabled = false;
};

using ExporterFactory = IExporter* (*)();
