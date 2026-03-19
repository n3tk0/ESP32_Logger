#pragma once
#include "../ISensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// WaterFlowSensor — Hall-effect water flow sensor (YF-S201 or YF-S403)
//
// Model differences:
//   YF-S201: 1–30 L/min range, ~450 pulses/L (default)
//   YF-S403: 1–60 L/min range, ~600 pulses/L (default, larger pipe)
//
// Config keys:
//   "pin"              — GPIO pin (default 21)
//   "pulses_per_liter" — override default PPL for the chosen model
//   "calibration"      — multiplier for volume/flow correction (default 1.0)
//   "read_interval_ms" — snapshot interval (default 1000 ms)
//   "cal_offset"       — additive offset on flow_rate (default 0.0)
//   "cal_scale"        — extra scale on flow_rate (default 1.0, stacks with calibration)
//
// Produces 2 metrics per read:
//   "flow_rate"  L/min  — instantaneous
//   "volume"     L      — cumulative since last reset
// ============================================================================
class WaterFlowSensor : public ISensor {
public:
    // Constructed by factory: pass type name and model-default PPL
    WaterFlowSensor(const char* typeName, float defaultPPL)
        : _typeName(typeName), _defaultPPL(defaultPPL) {}

    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;
    void resetVolume();

    const char* getType() const override { return _typeName; }
    const char* getName() const override {
        return (_typeName[4] == '4') ? "YF-S403 Water Flow"
                                     : "YF-S201 Water Flow";
    }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "flow_rate", "volume" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

    uint32_t rawPulseCount() const;

private:
    static void IRAM_ATTR _isr(void* arg);

    volatile uint32_t _pulses        = 0;
    volatile uint32_t _lastPulseUs   = 0;
    uint32_t          _lastPulseSnap = 0;
    uint32_t          _lastReadMs    = 0;

    const char* _typeName;
    float       _defaultPPL;
    float       _pulsesPerLiter = 450.0f;
    float       _calibration    = 1.0f;  // legacy volume multiplier
    uint32_t    _intervalMs     = 1000;
    int         _pin            = 21;

    CalibrationAxis _calFlow;   // applied to flow_rate
    CalibrationAxis _calVolume; // applied to volume

    static constexpr unsigned long ISR_DEBOUNCE_US = 1000; // 1ms
};
