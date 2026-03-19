#pragma once
#include "../ISensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// YF-S201 — Water flow sensor (pulse counter, Hall effect)
// Wraps the existing ISR-based pulse counting logic from the original firmware
// but encapsulates it as a clean plugin.
//
// Config keys:
//   "pin"              — GPIO pin (default 21)
//   "pulses_per_liter" — (default 450.0)
//   "calibration"      — multiplier (default 1.0)
//   "read_interval_ms" — how often to snapshot pulse count (default 1000)
//
// Produces 2 metrics per read:
//   "flow_rate"  L/min  — instantaneous (pulses in last interval)
//   "volume"     L      — cumulative since last reset
// ============================================================================
class YFS201Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;
    void resetVolume();

    const char* getType() const override { return "yfs201"; }
    const char* getName() const override { return "YF-S201 Water Flow"; }
    int getMetrics(const char** m, int max) const override {
        static const char* M[] = {"flow_rate","volume"};
        int n = 2; if(n>max) n=max;
        for(int i=0;i<n;i++) m[i]=M[i]; return n;
    }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }

    // Expose raw pulse count for legacy state machine compatibility
    uint32_t rawPulseCount() const;

private:
    static void IRAM_ATTR _isr(void* arg);

    volatile uint32_t _pulses        = 0;       // ISR increments
    volatile uint32_t _lastPulseUs   = 0;       // ISR debounce
    uint32_t          _lastPulseSnap = 0;       // snapshot at last read
    uint32_t          _lastReadMs    = 0;

    float    _pulsesPerLiter = 450.0f;
    float    _calibration    = 1.0f;
    uint32_t _intervalMs     = 1000;
    int      _pin            = 21;

    // ISR debounce: 1ms (same as original firmware)
    static constexpr unsigned long ISR_DEBOUNCE_US = 1000;
};
