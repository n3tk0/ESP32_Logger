#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include "../../drivers/BME280_Mini.h"

// ============================================================================
// BME280 / BMP280 — Temperature / Humidity / Pressure (I2C)
// Uses internal BME280_Mini driver (no Adafruit dependency).
// Auto-detects whether the chip is a BMP280 (no humidity sensor) or BME280.
// When a BMP280 is found, humidity reads are skipped.
//
// Config keys:
//   "sda", "scl"                     — I2C pins (defaults to Wire defaults)
//   "address"                         — 0x76 (default) or 0x77
//   "read_interval_ms"                — polling interval (default 10000)
//   "calibration" : {
//       "temperature": {"offset": 0.0, "scale": 1.0},
//       "humidity":    {"offset": 0.0, "scale": 1.0},  // BME280 only
//       "pressure":    {"offset": 0.0, "scale": 1.0}
//   }
//
// Produces 3 metrics (BME280) or 2 metrics (BMP280):
//   temperature (°C), humidity (%), pressure (hPa)
// ============================================================================
class BME280Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return _isBMP280 ? "bmp280" : "bme280"; }
    const char* getName() const override { return _isBMP280 ? "BMP280 Temp/Pressure" : "BME280 Environmental"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* mBME[] = { "temperature", "humidity", "pressure" };
        static const char* mBMP[] = { "temperature", "pressure" };
        if (_isBMP280) {
            int n = 2; if (n > maxOut) n = maxOut;
            for (int i = 0; i < n; i++) out[i] = mBMP[i];
            return n;
        }
        int n = 3; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = mBME[i];
        return n;
    }

private:
    BME280_Mini     _bme;
    uint32_t        _intervalMs = 10000;
    uint8_t         _addr       = 0x76;
    bool            _ready      = false;
    bool            _isBMP280   = false; // auto-detected at init

    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;
    CalibrationAxis _calPressure;

    SensorReading _makeReading(uint32_t ts, const char* metric,
                               float value, const char* unit) const;
};
