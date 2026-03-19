#pragma once
#include "../ISensor.h"

// ============================================================================
// Capacitive Soil Moisture Sensor v2 — analog ADC
//
// The sensor outputs an analog voltage inversely proportional to moisture:
//   - dry soil / in air  → high ADC value (around 3300 for 3.3V ADC)
//   - saturated soil     → low ADC value (around 1500)
//
// Config keys:
//   "pin"              — analog GPIO pin (default 0, ADC1 channel on ESP32)
//   "dry_value"        — ADC reading in completely dry air (default 3300)
//   "wet_value"        — ADC reading fully submerged in water (default 1500)
//   "adc_samples"      — number of ADC samples to average (default 16)
//   "read_interval_ms" — polling interval (default 5000)
//   "calibration": {"moisture": {"offset": 0.0, "scale": 1.0}}
//
// Produces 2 metrics:
//   "moisture_pct" — soil moisture percentage 0–100 %
//   "moisture_raw" — raw ADC count (for calibration reference)
// ============================================================================
class SoilMoistureSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "soil_moisture"; }
    const char* getName() const override { return "Capacitive Soil Moisture v2"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "moisture_pct", "moisture_raw" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    int      _pin        = 0;
    int      _dryValue   = 3300; // ADC value in dry air
    int      _wetValue   = 1500; // ADC value in water
    int      _samples    = 16;
    uint32_t _intervalMs = 5000;
    bool     _ready      = false;

    CalibrationAxis _calMoisture;
};
