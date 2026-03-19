#pragma once
#include "../ISensor.h"

// ============================================================================
// HC-SR04 — Ultrasonic distance sensor
//
// Trigger: pulse TRIG pin HIGH for ≥10µs
// Echo:    measure HIGH pulse width on ECHO pin
// Distance: cm = echo_us * 0.034 / 2  (speed of sound ~340 m/s)
//
// Config keys:
//   "trig_pin"         — trigger GPIO (default 5)
//   "echo_pin"         — echo GPIO (default 4)
//   "max_distance_cm"  — readings above this are ignored (default 400)
//   "read_interval_ms" — polling interval (default 1000)
//   "calibration": {"distance": {"offset": 0.0, "scale": 1.0}}
//
// Produces 1 metric: "distance" (cm)
// ============================================================================
class HCSR04Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "hcsr04"; }
    const char* getName() const override { return "HC-SR04 Distance"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "distance" };
        int n = 1; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    float _measureCm();

    int      _trigPin      = 5;
    int      _echoPin      = 4;
    float    _maxDistanceCm = 400.0f;
    uint32_t _intervalMs   = 1000;
    bool     _ready        = false;

    CalibrationAxis _calDistance;

    // Timeout: 400cm → ~23ms echo, add margin → 30ms
    static constexpr unsigned long ECHO_TIMEOUT_US = 30000;
};
