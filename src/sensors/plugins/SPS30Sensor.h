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
// DEVICE HEALTH
// -------------
// The PM values alone cannot tell you the sensor is healthy. The SPS30 derives
// mass concentration from a particle count over an assumed sample volume, so a
// fan that is slow, obstructed or seized keeps producing plausible, in-range,
// CRC-valid numbers that are simply wrong. This is the dominant silent-failure
// mode outdoors, and it gets more likely in the cold: below the sensor's
// -10 °C lower operating limit, bearing lubricant stiffens and the fan may not
// reach its commanded speed.
//
// So the driver polls the device status register (0xD206) and publishes the
// decoded flags as a "device_status" metric. Alert on `device_status > 0`.
//
// FAN CLEANING
// ------------
// The SPS30 runs an automatic fan-clean cycle every `auto_clean_interval_s`
// seconds of measurement runtime (device default 604800 = 7 days). Writing the
// interval RESETS the device's internal counter, so this driver reads the
// stored value first and writes only when it actually differs — otherwise
// every reboot would postpone cleaning by another full interval.
//
// Config keys:
//   "sda", "scl"             — I2C pins
//   "read_interval_ms"       — poll cadence (default 5000)
//   "status_interval_ms"     — device-status poll cadence (default 60000).
//                              Kept much slower than the PM cadence: the
//                              status register costs an extra I2C round trip
//                              and fan faults are not sub-minute events.
//   "auto_clean_interval_s"  — fan auto-clean interval in seconds.
//                              -1 (default) = leave the device setting alone.
//                               0           = disable auto-cleaning.
//                              Valid range otherwise 10..604800.
//   "clean_on_boot"          — run one fan-clean cycle at init (default false).
//                              Takes ~10 s during which readings are skipped.
//   "calibration": {
//       "pm1":  {"offset": 0.0, "scale": 1.0},
//       "pm25": {"offset": 0.0, "scale": 1.0},
//       "pm4":  {"offset": 0.0, "scale": 1.0},
//       "pm10": {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 5 metrics:
//   "pm1"           µg/m³ — PM1.0 mass concentration
//   "pm25"          µg/m³ — PM2.5 mass concentration
//   "pm4"           µg/m³ — PM4.0 mass concentration
//   "pm10"          µg/m³ — PM10  mass concentration
//   "device_status" bitfield — 0 = healthy. See STATUS_* below.
// ============================================================================
class SPS30Sensor : public ISensor {
public:
    // Decoded "device_status" metric bits. These are the driver's own compact
    // encoding, NOT the raw 32-bit register layout — the raw register is
    // sparse (bits 4, 5 and 21) and would not survive a float metric intact.
    static constexpr uint8_t STATUS_OK          = 0x00;
    static constexpr uint8_t STATUS_FAN_ERROR   = 0x01;  // fan blocked or broken
    static constexpr uint8_t STATUS_LASER_ERROR = 0x02;  // laser current out of range
    static constexpr uint8_t STATUS_FAN_SPEED   = 0x04;  // fan speed out of range
    static constexpr uint8_t STATUS_READ_FAILED = 0x08;  // status register unreadable

    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "sps30"; }
    const char* getName() const override { return "Sensirion SPS30 PM"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "pm1", "pm25", "pm4", "pm10", "device_status" };
        int n = 5; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    bool    _sendCmd(uint16_t cmd);
    bool    _sendCmdArg(uint16_t cmd, uint16_t arg);
    bool    _sendCmdArg32(uint16_t cmd, uint32_t arg);
    bool    _readWords(uint16_t* words, int count);
    bool    _dataReady();
    bool    _readStatus(uint32_t& raw);
    void    _pollStatus();
    void    _applyAutoCleanInterval(int32_t wantSeconds);
    static  uint8_t _crc8(const uint8_t* data, size_t len);

    uint32_t _intervalMs       = 5000;
    uint32_t _statusIntervalMs = 60000;
    bool     _ready            = false;
    uint32_t _warmupUntilMs    = 0;
    uint32_t _nextStatusMs     = 0;
    uint8_t  _status           = STATUS_OK;   // last decoded status bitfield

    CalibrationAxis _calPm1;
    CalibrationAxis _calPm25;
    CalibrationAxis _calPm4;
    CalibrationAxis _calPm10;

    static constexpr uint8_t  ADDR                = 0x69;
    static constexpr uint16_t CMD_START_MEASURE   = 0x0010;
    static constexpr uint16_t CMD_STOP_MEASURE    = 0x0104;
    static constexpr uint16_t CMD_READ_DATA_READY = 0x0202;
    static constexpr uint16_t CMD_READ_MEASURED   = 0x0300;
    static constexpr uint16_t CMD_AUTOCLEAN_INTV  = 0x8004;  // r/w, 32-bit seconds
    static constexpr uint16_t CMD_START_FAN_CLEAN = 0x5607;
    static constexpr uint16_t CMD_WAKE            = 0x1103;
    static constexpr uint16_t CMD_READ_STATUS     = 0xD206;  // 32-bit status register
    static constexpr uint16_t ARG_FLOAT_FORMAT    = 0x0300;  // 0x03 = IEEE-754 float

    // Raw device status register bit positions (SPS30 datasheet §4.4).
    static constexpr uint8_t RAW_BIT_FAN_ERROR   = 4;
    static constexpr uint8_t RAW_BIT_LASER_ERROR = 5;
    static constexpr uint8_t RAW_BIT_FAN_SPEED   = 21;

    // A fan-clean cycle spins the fan to maximum for ~10 s; readings taken
    // during it are meaningless, so reuse the warm-up gate to skip them.
    static constexpr uint32_t FAN_CLEAN_MS = 12000;
};
