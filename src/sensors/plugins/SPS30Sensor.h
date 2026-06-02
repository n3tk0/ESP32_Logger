#pragma once
#include "../ISensor.h"
// <Wire.h> lives in the .cpp — not referenced in this header.

// ============================================================================
// SPS30 — Sensirion particulate-matter sensor (I2C)
// I2C address: 0x69 (fixed)
//
// Measures mass concentrations PM1.0 / PM2.5 / PM4.0 / PM10 (µg/m³), plus
// number concentrations and typical particle size. This driver exposes the
// four mass concentrations — the fields used for air-quality logging and the
// sensor.community / openSenseMap exporters.
//
// Uses raw I2C with Sensirion CRC-8 (poly=0x31, init=0xFF), 16-bit big-endian
// words. Measured values are requested in the IEEE-754 big-endian float output
// format and reassembled from CRC-checked word pairs.
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "read_interval_ms"     — poll cadence (default 5000)
//   "calibration": {
//       "pm1":  {"offset": 0.0, "scale": 1.0},
//       "pm25": {"offset": 0.0, "scale": 1.0},
//       "pm4":  {"offset": 0.0, "scale": 1.0},
//       "pm10": {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 4 metrics:
//   "pm1"  µg/m³ — PM1.0 mass concentration
//   "pm25" µg/m³ — PM2.5 mass concentration
//   "pm4"  µg/m³ — PM4.0 mass concentration
//   "pm10" µg/m³ — PM10  mass concentration
// ============================================================================
class SPS30Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "sps30"; }
    const char* getName() const override { return "Sensirion SPS30 PM"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "pm1", "pm25", "pm4", "pm10" };
        int n = 4; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    bool    _sendCmd(uint16_t cmd);
    bool    _sendCmdArg(uint16_t cmd, uint16_t arg);
    bool    _readWords(uint16_t* words, int count);
    bool    _dataReady();
    static  uint8_t _crc8(const uint8_t* data, size_t len);

    uint32_t _intervalMs    = 5000;
    bool     _ready         = false;
    uint32_t _warmupUntilMs = 0;

    CalibrationAxis _calPm1;
    CalibrationAxis _calPm25;
    CalibrationAxis _calPm4;
    CalibrationAxis _calPm10;

    static constexpr uint8_t  ADDR                = 0x69;
    static constexpr uint16_t CMD_START_MEASURE   = 0x0010;
    static constexpr uint16_t CMD_STOP_MEASURE    = 0x0104;
    static constexpr uint16_t CMD_READ_DATA_READY = 0x0202;
    static constexpr uint16_t CMD_READ_MEASURED   = 0x0300;
    static constexpr uint16_t CMD_WAKE            = 0x1103;
    static constexpr uint16_t ARG_FLOAT_FORMAT    = 0x0300;  // 0x03 = IEEE-754 float
};
