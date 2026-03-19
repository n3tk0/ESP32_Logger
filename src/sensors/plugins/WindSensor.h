#pragma once
#include "../ISensor.h"

// ============================================================================
// WindSensor — cup anemometer + optional AH49E hall-effect wind vane
//
// Speed config keys:
//   "pin"              — anemometer GPIO (default 8)
//   "pulses_per_rev"   — hall pulses per revolution (default 1.0)
//   "meters_per_rev"   — effective circumference in m (default 0.5)
//   "sample_window_ms" — counting window for speed (default 3000)
//   "calibration": {"wind_speed": {"offset": 0.0, "scale": 1.0}}
//
// Direction config keys (optional — omit dir_pin to disable):
//   "dir_pin"          — analog GPIO for AH49E hall sensor (-1 = disabled)
//   "dir_min_val"      — ADC raw value at 0° / North (calibration min, default 0)
//   "dir_max_val"      — ADC raw value at 360°       (calibration max, default 4095)
//   Angle formula: angle = map(analogRead(dir_pin), dir_min_val, dir_max_val, 0, 360)
//
// Produces:
//   "wind_speed"     m/s   — always
//   "wind_direction" °     — only when dir_pin >= 0
// ============================================================================
class WindSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "wind"; }
    const char* getName() const override { return "Wind Speed/Direction"; }
    uint32_t    getReadIntervalMs() const override { return _sampleWindowMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "wind_speed", "wind_direction" };
        int n = (_dirPin >= 0) ? 2 : 1;
        if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    static void IRAM_ATTR _isr(void* arg);

    volatile uint32_t _pulses      = 0;
    volatile uint32_t _lastPulseUs = 0;

    float    _pulsesPerRev   = 1.0f;
    float    _metersPerRev   = 0.5f;
    uint32_t _sampleWindowMs = 3000;
    int      _pin            = 8;

    // AH49E direction sensor
    int      _dirPin    = -1;   // analog pin, -1 = disabled
    int      _dirMinVal = 0;    // ADC value at 0° (calibration)
    int      _dirMaxVal = 4095; // ADC value at 360° (calibration)

    CalibrationAxis _calSpeed;

    static constexpr uint32_t ISR_DEBOUNCE_US = 5000; // 5ms
};
