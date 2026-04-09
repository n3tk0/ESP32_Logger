#pragma once
#include "../ISensor.h"

// ============================================================================
// ZMCT103C — AC Current Sensor (analog ADC sampling)
//
// The module uses a current transformer with a burden resistor to produce a
// sinusoidal signal centred at VCC/2.  RMS current is estimated using the
// same zero-DC standard-deviation method as the ZMPT101B voltage sensor.
//
// Config keys:
//   "pin"              — analog GPIO pin (default 1, must be ADC-capable)
//   "current_factor"   — scale factor from ADC RMS count to Arms (default 1.0)
//                        Calibrate with a clamp meter: factor = I_actual / rmsCount.
//   "adc_samples"      — number of ADC samples per measurement window (default 200)
//   "sample_period_us" — microseconds between each ADC sample (default 100)
//   "read_interval_ms" — polling interval in ms (default 1000)
//   "calibration": {"current_arms": {"offset": 0.0, "scale": 1.0}}
//
// Produces 2 metrics:
//   "current_arms" — RMS AC current estimate (A)
//   "current_raw"  — raw ADC RMS count (for calibration)
// ============================================================================
class ZMCT103CSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "zmct103c"; }
    const char* getName() const override { return "ZMCT103C AC Current"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "current_arms", "current_raw" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    int      _pin           = 1;
    float    _currentFactor = 1.0f; // multiply ADC RMS count → Arms
    int      _samples       = 200;
    uint32_t _samplePeriodUs = 100;
    uint32_t _intervalMs    = 1000;
    bool     _ready         = false;

    CalibrationAxis _calCurrent;
};
