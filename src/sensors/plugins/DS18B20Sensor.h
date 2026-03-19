#pragma once
#include "../ISensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================================
// DS18B20 — 1-Wire digital temperature sensor
// Supports multiple sensors on a single bus (up to 8 reported).
// Each sensor on the bus gets its own metric named "temperature_0",
// "temperature_1", etc.
//
// Config keys:
//   "pin"              — 1-Wire data GPIO pin (default 2)
//   "resolution"       — 9/10/11/12 bits (default 12 = 0.0625°C, ~750ms)
//   "read_interval_ms" — polling interval (default 5000)
//   "calibration": {
//       "temperature": {"offset": 0.0, "scale": 1.0}  // applied to all probes
//   }
//
// Produces 1–8 metrics (one per detected sensor):
//   "temperature"   °C  — single sensor (first found)
//   "temperature_1" °C  — second sensor (if present)
//   ...up to "temperature_7"
// ============================================================================
class DS18B20Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType()    const override { return "ds18b20"; }
    const char* getName()    const override { return "DS18B20 Temperature"; }
    bool        isBlocking() const override { return true; }  // delay() up to 760ms during conversion
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override;

private:
    OneWire*          _ow      = nullptr;
    DallasTemperature* _dt     = nullptr;

    uint32_t _intervalMs  = 5000;
    uint8_t  _resolution  = 12;
    int      _count       = 0;  // number of sensors found on bus
    bool     _ready       = false;

    CalibrationAxis _calTemp;

    static constexpr int MAX_SENSORS = 8;
    static const char* _metricName(int idx);
};
