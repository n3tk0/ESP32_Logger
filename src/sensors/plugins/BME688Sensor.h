#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include "../../drivers/BME688_Mini.h"

// ============================================================================
// BME688 — Temperature / Humidity / Pressure / Gas Resistance (I2C)
// Uses internal BME688_Mini driver (no Adafruit dependency).
// Compatible with BME680 and BME688 for basic readings.
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
    BME688_Mini     _bme;
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
