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
    virtual uint32_t    retryDelayMs() const { return 1000; }

    // EFFECTIVE send interval in ms, as resolved by ExportManager from the
    // exporter's own "interval_ms" or, when that is absent/0, from the common
    // export.defaults.interval_ms. Exporters read it; they don't set it.
    virtual uint32_t    intervalMs()   const { return _intervalMs; }
    void                setIntervalMs(uint32_t ms) { _intervalMs = ms; }

    // A floor the remote API imposes (sensor.community: 145 s). ExportManager
    // clamps the configured interval up to this, so a UI typo can't get the
    // device rate-limited or banned.
    virtual uint32_t    minIntervalMs() const { return 0; }

    // Whether a batch that exhausted its retries is worth writing to the spool
    // and replaying later. Only true for APIs that accept the reading's own
    // timestamp — replaying into one that stamps arrival time just puts old
    // values at "now".
    virtual bool        spoolOnFailure() const { return true; }

    // Event-driven exporters (webhook threshold rules) see every reading the
    // moment it arrives instead of the periodic latest-value snapshot. The
    // interval and sensor selection do not apply to them.
    virtual bool        isStreaming() const { return false; }

protected:
    bool     _enabled    = false;
    uint32_t _intervalMs = 0;
};

using ExporterFactory = IExporter* (*)();
