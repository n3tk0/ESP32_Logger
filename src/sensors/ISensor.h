#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "../core/SensorTypes.h"

// ============================================================================
// CalibrationAxis — per-metric calibration helper (offset + scale)
// Applies: calibrated = raw * scale + offset
// JSON config: {"offset": 0.0, "scale": 1.0}
// ============================================================================
struct CalibrationAxis {
    float offset = 0.0f;
    float scale  = 1.0f;
    float apply(float raw) const { return raw * scale + offset; }
    void  load(JsonObjectConst parent, const char* key) {
        JsonObjectConst c = parent[key];
        if (!c.isNull()) {
            offset = c["offset"] | 0.0f;
            scale  = c["scale"]  | 1.0f;
        }
    }
};

// ============================================================================
// ISensor — abstract plugin interface
// Every sensor driver must implement this and be registered with SensorManager.
// ============================================================================
class ISensor {
public:
    virtual ~ISensor() = default;

    // ------------------------------------------------------------------
    // init()
    //   Called once at boot with the sensor's JSON config object.
    //   Example config:
    //     {"id":"env_indoor","type":"bme280","enabled":true,
    //      "interface":"i2c","sda":6,"scl":7,"address":118,
    //      "read_interval_ms":10000}
    //   Return true if hardware is ready.
    // ------------------------------------------------------------------
    virtual bool init(JsonObjectConst config) = 0;

    // ------------------------------------------------------------------
    // read()
    //   Fill a single SensorReading and return true on success.
    //   For multi-metric sensors prefer readAll().
    // ------------------------------------------------------------------
    virtual bool read(SensorReading& out) = 0;

    // ------------------------------------------------------------------
    // readAll()
    //   Fill up to maxOut readings (one per metric).
    //   Default implementation calls read() once.
    //   Override for sensors that produce multiple metrics per sample.
    // ------------------------------------------------------------------
    virtual int readAll(SensorReading* out, int maxOut) {
        if (maxOut < 1) return 0;
        return read(out[0]) ? 1 : 0;
    }

    // Plugin type string — must be stable across builds (used in JSON keys)
    virtual const char* getType() const = 0;

    // Human-readable display name
    virtual const char* getName() const = 0;

    // Milliseconds between consecutive reads (0 = as fast as possible)
    virtual uint32_t getReadIntervalMs() const { return 5000; }

    // True if the sensor requires a continuous background task
    // (e.g. UART-streaming sensors like SDS011 / PMS5003)
    virtual bool isContinuous() const { return false; }

    // Returns metric names this sensor produces (e.g. "temperature", "humidity").
    // Fills out[] with up to maxOut pointers to static string literals.
    // Returns the number of metrics filled.
    virtual int getMetrics(const char** out, int maxOut) const { return 0; }

    // Runtime enable/disable (web UI toggle without reboot)
    virtual void setEnabled(bool en) { _enabled = en; }
    virtual bool isEnabled()   const { return _enabled; }

    // Sensor instance id (set by SensorManager from JSON "id" field)
    const char* getId() const { return _id; }
    void        setId(const char* id) { strncpy(_id, id, sizeof(_id)-1); }

    // Last successful read timestamp (Unix epoch)
    uint32_t    lastReadTs()               const { return _lastReadTs; }
    void        setLastReadTs(uint32_t ts)       { _lastReadTs = ts; }

protected:
    bool     _enabled    = false;
    char     _id[17]     = {};
    uint32_t _lastReadTs = 0;
};

// Factory function signature — used to register plugins without vtable overhead
using SensorFactory = ISensor* (*)();
