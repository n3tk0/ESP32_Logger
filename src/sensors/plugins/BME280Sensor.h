#pragma once
#include "../ISensor.h"
#include <Wire.h>   // TwoWire (resolved bus handle stored below)
// <Wire.h> moved to BME280Sensor.cpp (also pulled transitively via BME280_Mini.h).
#include "../../drivers/BME280_Mini.h"

// ============================================================================
// BME280 / BMP280 — Temperature / Humidity / Pressure (I2C)
// Uses internal BME280_Mini driver (no Adafruit dependency).
// Auto-detects whether the chip is a BMP280 (no humidity sensor) or BME280.
// When a BMP280 is found, humidity reads are skipped.
//
// Config keys:
//   "sda", "scl"                     — I2C pins (defaults to Wire defaults)
//   "bus"                  — I2C controller: 0 (default) or 1. Devices with
//                            the same fixed address must sit on different
//                            buses. Bus 1 needs a chip with two I2C
//                            controllers (S3/ESP32; the C3 has one).
//   "address"                         — 0x76 (default) or 0x77
//   "read_interval_ms"                — polling interval (default 10000)
//   "calibration" : {
//       "temperature": {"offset": 0.0, "scale": 1.0},
//       "humidity":    {"offset": 0.0, "scale": 1.0},  // BME280 only
//       "pressure":    {"offset": 0.0, "scale": 1.0}
//   }
//   "ambient_temp_sensor"  — id of a sensor giving the TRUE air temperature,
//                            for the self-heating correction below. Empty
//                            (default) uses this sensor's own CALIBRATED
//                            temperature, which is the right answer when the
//                            self-heating has been measured into
//                            calibration.temperature.offset.
//   "ambient_temp_metric"  — metric on that sensor (default "temperature")
//   "ambient_max_age_ms"   — reject the reference above this age (default
//                            60000). A frozen reference is worse than none.
//
// SELF-HEATING, AND WHY AN OFFSET ALONE IS NOT ENOUGH
// ---------------------------------------------------
// A BME280 bolted to a board that runs WiFi reads warm. A temperature offset
// fixes the temperature and leaves the humidity wrong, because relative
// humidity is relative TO a temperature: air at a fixed absolute moisture
// content reads lower RH the warmer the sensor measuring it — around 6 %
// relative per °C near room temperature. Correcting only the temperature
// therefore reports a room drier than it is, confidently.
//
// The way out is the dew point. RH measured at the die temperature, paired
// with that same die temperature, gives the TRUE dew point: it is a property
// of the air's absolute moisture and is invariant under heating the sensor.
// Re-express that dew point at the real air temperature and the RH comes back.
//
// This is the correction BME688Sensor has had since it shipped; it is the same
// maths and the same Psychrometrics helpers, only the die temperature comes
// from the ordinary reading rather than from a gas heater's cycle.
//
// Produces up to 5 metrics (BME280) or 2 (BMP280):
//   temperature (°C), humidity (%), pressure (hPa),
//   dew_point (°C)        — true ambient dew point, invariant under heating
//   humidity_ambient (%)  — RH implied by that dew point at the ambient
//                           temperature; equals humidity when there is no
//                           self-heating to correct for
//
// The derived pair is dropped rather than emitted as NaN when the inputs are
// out of range, so a bad reading never lands in storage as a permanent null.
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
        static const char* mBME[] = { "temperature", "humidity", "pressure",
                                      "dew_point", "humidity_ambient" };
        static const char* mBMP[] = { "temperature", "pressure" };
        if (_isBMP280) {
            int n = 2; if (n > maxOut) n = maxOut;
            for (int i = 0; i < n; i++) out[i] = mBMP[i];
            return n;
        }
        int n = 5; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = mBME[i];
        return n;
    }

private:
    // Resolved I2C bus. Every transfer goes through _wire rather than the
    // global Wire, so this sensor can sit on either controller.
    uint8_t  _bus  = 0;
    TwoWire* _wire = nullptr;
    BME280_Mini     _bme;
    uint32_t        _intervalMs = 10000;
    uint8_t         _addr       = 0x76;
    bool            _ready      = false;
    bool            _isBMP280   = false; // auto-detected at init

    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;
    CalibrationAxis _calPressure;

    // Ambient temperature reference for the self-heating correction. Empty
    // means "use my own calibrated temperature", which is correct when the
    // self-heating lives in calibration.temperature.offset.
    char     _ambientSensor[17] = {};
    char     _ambientMetric[16] = "temperature";
    uint32_t _ambientMaxAgeMs   = 60000;
    mutable bool _ambientWarned = false;   // one line per outage, not per read

    /// True air temperature to express the humidity against, or `fallbackC`
    /// when the configured reference is missing, stale or implausible.
    float _ambientTempC(float fallbackC) const;

    SensorReading _makeReading(uint32_t ts, const char* metric,
                               float value, const char* unit) const;
};
