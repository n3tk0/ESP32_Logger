#pragma once
#include "../ISensor.h"
#include <Wire.h>

// ============================================================================
// SGP30 — TVOC / eCO2 sensor (I2C, Sensirion)
// Config keys: "sda", "scl", "read_interval_ms" (default 30000)
// Address is fixed at 0x58 (no config needed).
// Produces 2 metrics: tvoc (ppb), eco2 (ppm)
//
// Note: SGP30 requires 15s warmup after power-on.
//       First 15 readings return baseline values (400ppm eCO2, 0ppb TVOC).
//       Humidity compensation: not implemented (requires external RH sensor).
// ============================================================================
class SGP30Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "sgp30"; }
    const char* getName() const override { return "SGP30 TVOC/eCO2"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "tvoc", "eco2" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    bool    _measure(uint16_t& tvoc, uint16_t& eco2);
    bool    _sendCommand(uint16_t cmd);
    bool    _readWords(uint16_t* words, int count);
    static uint8_t _crc8(uint8_t d1, uint8_t d2);

    uint32_t _intervalMs = 30000;
    bool     _ready      = false;
    uint32_t _initMs     = 0;

    CalibrationAxis _calTvoc;
    CalibrationAxis _calEco2;

    static constexpr uint8_t  ADDR          = 0x58;
    static constexpr uint16_t CMD_IAQ_INIT  = 0x2003;
    static constexpr uint16_t CMD_IAQ_MEAS  = 0x2008;
    static constexpr uint16_t CMD_GET_FEAT  = 0x202F;
};
