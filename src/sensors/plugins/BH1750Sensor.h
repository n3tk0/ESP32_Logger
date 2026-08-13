#pragma once
#include "../ISensor.h"
#include <Wire.h>   // TwoWire (resolved bus handle stored below)

// ============================================================================
// BH1750 — Digital ambient light sensor (I2C)
//
// Config keys:
//   "sda", "scl"       — I2C pins
//   "bus"                  — I2C controller: 0 (default) or 1. Devices with
//                            the same fixed address must sit on different
//                            buses. Bus 1 needs a chip with two I2C
//                            controllers (S3/ESP32; the C3 has one).
//   "address"          — 0x23 (ADDR=LOW, default) or 0x5C (ADDR=HIGH)
//   "mode"             — "H" = high res 1 lx (default), "H2" = 0.5 lx, "L" = 4 lx
//   "read_interval_ms" — polling interval (default 2000)
//   "calibration": {"lux": {"offset": 0.0, "scale": 1.0}}
//
// Produces 1 metric: "lux" (lx)
// ============================================================================
class BH1750Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "bh1750"; }
    const char* getName() const override { return "BH1750 Light"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "lux" };
        int n = 1; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    // Resolved I2C bus. Every transfer goes through _wire rather than the
    // global Wire, so this sensor can sit on either controller.
    uint8_t  _bus  = 0;
    TwoWire* _wire = nullptr;
    bool _sendCmd(uint8_t cmd);
    bool _readLux(float& lux);

    uint8_t  _addr       = 0x23;
    uint32_t _intervalMs = 2000;
    uint8_t  _modeCmd    = 0x10; // CONT_H_RES_MODE
    float    _divider    = 1.2f; // sensitivity: raw / 1.2 = lux (H mode)
    bool     _ready      = false;

    CalibrationAxis _calLux;

    // BH1750 commands
    static constexpr uint8_t CMD_POWER_ON  = 0x01;
    static constexpr uint8_t CMD_RESET     = 0x07;
    static constexpr uint8_t CMD_CONT_H    = 0x10; // continuous, 1 lx res, ~120ms
    static constexpr uint8_t CMD_CONT_H2   = 0x11; // continuous, 0.5 lx res, ~120ms
    static constexpr uint8_t CMD_CONT_L    = 0x13; // continuous, 4 lx res, ~16ms
};
