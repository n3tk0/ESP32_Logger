#pragma once
#include "../ISensor.h"
#include <Wire.h>   // TwoWire (resolved bus handle stored below)

// ============================================================================
// VEML6075 — UV-A / UV-B / UV Index sensor (I2C)
// I2C address: 0x10 (fixed)
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "bus"                  — I2C controller: 0 (default) or 1. Devices with
//                            the same fixed address must sit on different
//                            buses. Bus 1 needs a chip with two I2C
//                            controllers (S3/ESP32; the C3 has one).
//   "read_interval_ms"     — polling interval (default 15000)
//   "calibration": {
//       "uva":      {"offset": 0.0, "scale": 1.0},
//       "uvb":      {"offset": 0.0, "scale": 1.0},
//       "uv_index": {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 3 metrics:
//   "uva"      — UV-A counts
//   "uvb"      — UV-B counts
//   "uv_index" — calculated UV index (0–11+)
// ============================================================================
class VEML6075Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "veml6075"; }
    const char* getName() const override { return "VEML6075 UV"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "uva", "uvb", "uv_index" };
        int n = 3; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    // Resolved I2C bus. Every transfer goes through _wire rather than the
    // global Wire, so this sensor can sit on either controller.
    uint8_t  _bus  = 0;
    TwoWire* _wire = nullptr;
    bool    _writeReg16(uint8_t reg, uint16_t val);
    bool    _readReg16(uint8_t reg, uint16_t& val);

    uint32_t _intervalMs = 15000;
    bool     _ready      = false;

    CalibrationAxis _calUva;
    CalibrationAxis _calUvb;
    CalibrationAxis _calUvIndex;

    // VEML6075 I2C registers
    static constexpr uint8_t ADDR      = 0x10;
    static constexpr uint8_t REG_CONF  = 0x00; // configuration
    static constexpr uint8_t REG_UVA   = 0x07; // UVA raw
    static constexpr uint8_t REG_UVB   = 0x09; // UVB raw
    static constexpr uint8_t REG_COMP1 = 0x0A; // UV COMP1 (noise compensation)
    static constexpr uint8_t REG_COMP2 = 0x0B; // UV COMP2 (noise compensation)

    // UV index correction coefficients (from VEML6075 app note AN84367)
    static constexpr float UVI_UVA_VIS_COEFF = 2.22f;
    static constexpr float UVI_UVA_IR_COEFF  = 1.33f;
    static constexpr float UVI_UVB_VIS_COEFF = 2.95f;
    static constexpr float UVI_UVB_IR_COEFF  = 1.74f;
    static constexpr float UVI_UVA_RESP      = 0.001461f;
    static constexpr float UVI_UVB_RESP      = 0.002591f;
};
