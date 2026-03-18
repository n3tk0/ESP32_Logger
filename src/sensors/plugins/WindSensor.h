#pragma once
#include "../ISensor.h"

// ============================================================================
// WindSensor — cup anemometer (pulse frequency → wind speed)
// Config keys: "pin", "pulses_per_rev" (default 1),
//              "meters_per_rev" (default 0.5m circumference-equivalent),
//              "sample_window_ms" (default 3000)
// Produces 1 metric: "wind_speed" in m/s
// ============================================================================
class WindSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "wind"; }
    const char* getName() const override { return "Wind Speed"; }
    uint32_t    getReadIntervalMs() const override { return _sampleWindowMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "wind_speed" };
        int n = 1; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    static void IRAM_ATTR _isr(void* arg);

    volatile uint32_t _pulses      = 0;
    volatile uint32_t _lastPulseUs = 0;

    float    _pulsesPerRev   = 1.0f;
    float    _metersPerRev   = 0.5f;   // ~1.0m circumference / 2 poles
    uint32_t _sampleWindowMs = 3000;
    int      _pin            = 8;

    static constexpr uint32_t ISR_DEBOUNCE_US = 5000; // 5ms
};
