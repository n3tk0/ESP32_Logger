#pragma once
#include "../ISensor.h"

// ============================================================================
// ZMPT101B — AC Voltage Sensor (analog ADC sampling)
//
// The module outputs a sinusoidal signal centred at VCC/2.
// RMS voltage is estimated by sampling the ADC at high rate and computing
// the standard deviation of the AC component (zero-DC method).
//
// Config keys:
//   "pin"              — analog GPIO pin (default 0, must be ADC-capable)
//   "voltage_factor"   — scale factor from ADC RMS count to Vrms (default 1.0)
//                        Calibrate by comparing against a known AC voltage source.
//   "adc_samples"      — number of ADC samples per measurement window (default 200)
//   "sample_period_us" — microseconds between each ADC sample (default 100)
//   "read_interval_ms" — polling interval in ms (default 1000)
//   "calibration": {"voltage_vrms": {"offset": 0.0, "scale": 1.0}}
//
// Produces 2 metrics:
//   "voltage_vrms" — RMS AC voltage estimate (V)
//   "voltage_raw"  — raw ADC RMS count (for calibration)
// ============================================================================
class ZMPT101BSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "zmpt101b"; }
    const char* getName() const override { return "ZMPT101B AC Voltage"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "voltage_vrms", "voltage_raw" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    int      _pin          = 0;
    float    _voltageFactor = 1.0f; // multiply ADC RMS count → Vrms
    int      _samples      = 200;
    uint32_t _samplePeriodUs = 100;
    uint32_t _intervalMs   = 1000;
    bool     _ready        = false;

    CalibrationAxis _calVoltage;
};
