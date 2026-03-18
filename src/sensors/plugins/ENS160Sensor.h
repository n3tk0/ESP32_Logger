#pragma once
#include "../ISensor.h"
#include <Wire.h>

// ============================================================================
// ENS160 — TVOC / eCO2 / AQI sensor (I2C, ScioSense)
// Config keys: "sda", "scl", "address" (0x52 or 0x53), "read_interval_ms"
// Produces 3 metrics: tvoc (ppb), eco2 (ppm), aqi (1-5)
// Requires Adafruit_ENS160 or ScioSense_ENS160 library.
// ============================================================================
class ENS160Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "ens160"; }
    const char* getName() const override { return "ENS160 TVOC/eCO2"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "tvoc", "eco2", "aqi" };
        int n = 3; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    // Use raw I2C registers to avoid library dependency issues.
    // Based on ScioSense ENS160 datasheet (v1.3).
    bool    _writeReg(uint8_t reg, uint8_t val);
    bool    _readRegs(uint8_t reg, uint8_t* buf, size_t len);
    bool    _waitReady(uint32_t timeoutMs = 1000);

    uint8_t  _addr      = 0x52;
    uint32_t _intervalMs = 30000;
    bool     _ready     = false;

    static constexpr uint8_t REG_OPMODE   = 0x10;
    static constexpr uint8_t REG_STATUS   = 0x20;
    static constexpr uint8_t REG_AQI      = 0x21;
    static constexpr uint8_t REG_TVOC_L   = 0x22;
    static constexpr uint8_t REG_ECO2_L   = 0x24;
    static constexpr uint8_t MODE_STANDARD = 0x02;
    static constexpr uint8_t MODE_IDLE     = 0x01;
};
