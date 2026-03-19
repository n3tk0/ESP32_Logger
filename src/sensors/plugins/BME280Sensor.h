#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include <Adafruit_BME280.h>

// ============================================================================
// BME280 — Temperature / Humidity / Pressure (I2C)
// Config keys: "sda", "scl", "address" (default 0x76=118)
// Produces 3 metrics per readAll(): temperature, humidity, pressure
// ============================================================================
class BME280Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "bme280"; }
    const char* getName() const override { return "BME280 Environmental"; }
    int getMetrics(const char** m, int max) const override {
        static const char* M[] = {"temperature","humidity","pressure"};
        int n = 3; if(n>max) n=max;
        for(int i=0;i<n;i++) m[i]=M[i]; return n;
    }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }

private:
    Adafruit_BME280 _bme;
    uint32_t        _intervalMs = 10000;
    uint8_t         _addr       = 0x76;
    bool            _ready      = false;

    // Populate a reading for one metric
    SensorReading _makeReading(uint32_t ts, const char* metric,
                               float value, const char* unit) const;
};
