#pragma once
#include "../ISensor.h"
#include <Wire.h>

// ============================================================================
// VEML7700 — High-accuracy ambient light sensor (I2C)
// I2C address: 0x10 (fixed) — NOTE: conflicts with VEML6075 if both on same bus
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "gain"                 — 0=1x, 1=2x, 2=1/8x, 3=1/4x (default 0)
//   "integration_ms"       — 25/50/100/200/400/800 (default 100)
//   "read_interval_ms"     — polling interval (default 5000)
//   "calibration": {
//       "lux":   {"offset": 0.0, "scale": 1.0},
//       "white": {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 2 metrics:
//   "lux"   — ambient light in lux
//   "white" — white channel raw counts
// ============================================================================
class VEML7700Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "veml7700"; }
    const char* getName() const override { return "VEML7700 Ambient Light"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "lux", "white" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    bool    _writeReg(uint8_t reg, uint16_t val);
    bool    _readReg(uint8_t reg, uint16_t& val);
    float   _countsToLux(uint16_t counts) const;

    uint32_t _intervalMs = 5000;
    bool     _ready      = false;
    uint8_t  _gain       = 0;   // gain index 0–3
    uint16_t _intMs      = 100; // integration time ms

    // Resolution (lux/count) depends on gain + integration time
    float _resolution = 0.0042f; // default: gain=1x, IT=100ms

    CalibrationAxis _calLux;
    CalibrationAxis _calWhite;

    static constexpr uint8_t  ADDR     = 0x10;
    static constexpr uint8_t  REG_CONF = 0x00;
    static constexpr uint8_t  REG_ALS  = 0x04;
    static constexpr uint8_t  REG_WHITE = 0x05;
};
