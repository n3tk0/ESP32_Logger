#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include "../../drivers/BME688_Mini.h"

// ============================================================================
// BME680 / BME688 — Temperature / Humidity / Pressure / Gas + IAQ (I2C)
// Uses the internal BME688_Mini driver (no Adafruit / BSEC dependency).
// One driver backs both the "bme688" and "bme680" plugin ids — same I2C
// protocol and MOX gas element; only the reading's type tag differs.
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
// Produces 5 metrics:
//   "temperature"    °C
//   "humidity"       %
//   "pressure"       hPa
//   "gas_resistance" Ω    — MOX resistance (higher = cleaner air)
//   "iaq"            0..500 air-quality index (LOWER = cleaner; BSEC scale)
//                    derived from humidity + a self-calibrating gas baseline.
//                    NOTE: heuristic, not a Bosch-BSEC gas classification — it
//                    indicates overall air quality, not the specific gas type.
// ============================================================================
class BME688Sensor : public ISensor {
public:
    // `type` lets the same driver back both the "bme688" and "bme680" plugin
    // ids (identical I2C protocol / MOX gas element); it only tags the readings.
    explicit BME688Sensor(const char* type = "bme688") : _type(type) {}

    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return _type; }
    const char* getName() const override { return "BME680/688 Environmental+Gas"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    bool        isBlocking() const override { return true; }  // performReading() blocks up to 1 s
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "temperature", "humidity", "pressure", "gas_resistance", "iaq" };
        int n = 5; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    // IAQ (Indoor Air Quality) — derived 0..500 index (lower = cleaner air,
    // BSEC convention) from humidity + a self-calibrating gas-resistance
    // baseline. Heuristic only (no Bosch BSEC); accuracy ramps up over the
    // first minutes as the baseline settles.
    float _computeIaq(float humidity, float rawGasOhm);

    const char*  _type;                  // "bme688" or "bme680"
    BME688_Mini  _bme;
    uint32_t _intervalMs   = 10000;
    uint8_t  _addr         = 0x76;
    int      _heaterTemp   = 320;
    int      _heaterDurMs  = 150;
    bool     _ready        = false;
    float    _gasBaseline  = 0.0f;        // clean-air resistance ceiling (Ω)

    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;
    CalibrationAxis _calPressure;
    CalibrationAxis _calGas;
};
