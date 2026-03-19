#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include <Adafruit_BME680.h>

// ============================================================================
// BME688 — Temperature / Humidity / Pressure / Gas Resistance (I2C)
// Uses the Adafruit BME680 library which is compatible with BME688 for
// basic sensor readings (without Bosch BSEC AI features).
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "address"              — 0x76 (default) or 0x77
//   "read_interval_ms"     — polling interval (default 10000)
//   "heater_temp"          — gas heater target °C (default 320)
//   "heater_duration_ms"   — heater on duration (default 150)
//   "calibration": {
//       "temperature":    {"offset": 0.0, "scale": 1.0},
//       "humidity":       {"offset": 0.0, "scale": 1.0},
//       "pressure":       {"offset": 0.0, "scale": 1.0},
//       "gas_resistance": {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 4 metrics:
//   "temperature"    °C
//   "humidity"       %
//   "pressure"       hPa
//   "gas_resistance" Ω  — proxy for air quality (higher = cleaner air)
// ============================================================================
class BME688Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "bme688"; }
    const char* getName() const override { return "BME688 Environmental+Gas"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "temperature", "humidity", "pressure", "gas_resistance" };
        int n = 4; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    Adafruit_BME680 _bme;
    uint32_t _intervalMs   = 10000;
    uint8_t  _addr         = 0x76;
    int      _heaterTemp   = 320;
    int      _heaterDurMs  = 150;
    bool     _ready        = false;

    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;
    CalibrationAxis _calPressure;
    CalibrationAxis _calGas;
};
