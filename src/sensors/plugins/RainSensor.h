#pragma once
#include "../ISensor.h"

// ============================================================================
// RainSensor — tipping-bucket rain gauge (pulse + time-delta calculation)
// Config keys: "pin", "mm_per_pulse" (default 0.2794 mm = standard 0.011")
// Produces 2 metrics:
//   "rain_rate"  mm/h  — extrapolated from inter-pulse interval
//   "rain_total" mm    — cumulative since last reset
// ============================================================================
class RainSensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;
    void resetTotal();

    const char* getType() const override { return "rain"; }
    const char* getName() const override { return "Rain Gauge"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "rain_rate", "rain_total" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    static void IRAM_ATTR _isr(void* arg);

    volatile uint32_t _tips          = 0;
    volatile uint32_t _lastTipUs     = 0;
    volatile uint32_t _lastIntervalUs = 0; // µs between last two tips

    float    _mmPerTip   = 0.2794f;
    uint32_t _intervalMs = 60000; // report every minute
    int      _pin        = 9;

    static constexpr uint32_t ISR_DEBOUNCE_US = 20000; // 20ms debounce
};
