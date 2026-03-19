#pragma once
#include "../ISensor.h"
#include <Wire.h>

// ============================================================================
// SCD40 / SCD41 — CO2 / Temperature / Humidity sensor (I2C, Sensirion)
// I2C address: 0x62 (fixed)
//
// SCD40: CO2 range 400–2000 ppm, periodic measurement (5s interval)
// SCD41: CO2 range 400–5000 ppm, otherwise identical protocol
//
// Uses raw I2C with Sensirion CRC-8 (poly=0x31, init=0xFF).
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "read_interval_ms"     — how often to request a fresh reading (default 5000)
//   "calibration": {
//       "co2":         {"offset": 0.0, "scale": 1.0},
//       "temperature": {"offset": 0.0, "scale": 1.0},
//       "humidity":    {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 3 metrics:
//   "co2"         ppm  — CO2 concentration
//   "temperature" °C   — on-chip temperature
//   "humidity"    %    — relative humidity
// ============================================================================
class SCD4xSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "scd4x"; }
    const char* getName() const override { return "SCD40/41 CO2"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "co2", "temperature", "humidity" };
        int n = 3; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    bool    _sendCmd(uint16_t cmd);
    bool    _readWords(uint16_t* words, int count);
    bool    _dataReady();
    static  uint8_t _crc8(const uint8_t* data, size_t len);

    uint32_t _intervalMs = 5000;
    bool     _ready      = false;

    CalibrationAxis _calCo2;
    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;

    static constexpr uint8_t  ADDR                     = 0x62;
    static constexpr uint16_t CMD_START_PERIODIC        = 0x21B1;
    static constexpr uint16_t CMD_STOP_PERIODIC         = 0x3F86;
    static constexpr uint16_t CMD_READ_MEASUREMENT      = 0xEC05;
    static constexpr uint16_t CMD_GET_DATA_READY_STATUS = 0xE4B8;
    static constexpr uint16_t CMD_REINIT                = 0x3646;
};
