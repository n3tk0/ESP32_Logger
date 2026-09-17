#include "ZMPT101BSensor.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include <math.h>

// R28 / AUDIT 24.11: ADC_11db is deprecated in IDF 5.x (Arduino Core 3.x) in
// favour of ADC_ATTEN_DB_12 (same 0..3.1 V range). On older Arduino Core 2.x
// (IDF 4.x) the new symbol isn't defined, so fall back to ADC_11db.
#ifndef ADC_ATTEN_DB_12
#  define ADC_ATTEN_DB_12 ADC_11db
#endif

bool ZMPT101BSensor::init(JsonObjectConst cfg) {
    _enabled        = cfg["enabled"]          | true;
    _pin            = cfg["pin"]              | -1;  // R11: unset → init refuses
    _voltageFactor  = cfg["voltage_factor"]   | 1.0f;
    _samples        = cfg["adc_samples"]      | 200;
    _samplePeriodUs = cfg["sample_period_us"] | 100;
    _intervalMs     = cfg["read_interval_ms"] | 1000;

    if (_samples < 10)  _samples = 10;
    if (_samples > 500) _samples = 500;

    JsonObjectConst cal = cfg["calibration"];
    _calVoltage.load(cal, "voltage_vrms");

    if (!validateAttachPin(_pin, "zmpt101b", "pin")) return false;
    analogSetPinAttenuation(_pin, ADC_ATTEN_DB_12); // full-scale ~3.1 V
    pinMode(_pin, INPUT);
    _ready = true;

    Serial.printf("[ZMPT101B] pin=%d factor=%.4f samples=%d period_us=%u\n",
                  _pin, _voltageFactor, _samples, _samplePeriodUs);
    return true;
}

bool ZMPT101BSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int ZMPT101BSensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 2) return 0;

    // -----------------------------------------------------------------------
    // Zero-DC RMS estimation:
    //   1. Collect N samples with a fixed inter-sample delay.
    //   2. Compute mean (DC bias ≈ VCC/2 mid-rail).
    //   3. Subtract mean, square, sum → variance → √ → AC RMS count.
    //   4. Multiply by voltage_factor → Vrms.
    // -----------------------------------------------------------------------
    // ONE PASS, NO BUFFER. This used to keep every sample:
    //
    //     int* buf = (int*)alloca(sizeof(int) * samples);
    //
    // with `samples` clamped at 500, so up to 2000 bytes of stack — taken on
    // SlowSensorTask, whose whole stack is STACK_SLOW_SENSOR_TASK (4096
    // bytes), and taken from inside a call chain several frames deep.  alloca
    // cannot report failure: the overflow is a corrupted stack, on the task
    // that also drives the UART dust sensors.
    //
    // Welford's algorithm needs no samples kept and is better conditioned
    // than the mean-then-subtract pass it replaces (verified against it on
    // synthetic ADC sweeps: agreement to ~4e-16 relative, i.e. double
    // rounding).  m2 accumulates the sum of squared deviations, so dividing
    // by `samples` gives the same population variance as before.
    const int samples = _samples;
    double mean = 0.0, m2 = 0.0;

    for (int i = 0; i < samples; i++) {
        const double x = (double)analogRead(_pin);
        const double d = x - mean;
        mean += d / (double)(i + 1);
        m2   += d * (x - mean);
        delayMicroseconds(_samplePeriodUs);
    }
    double rmsCount = sqrt(m2 / (double)samples);
    float vrms = (float)(rmsCount * _voltageFactor);
    vrms = _calVoltage.apply(vrms);
    if (vrms < 0.0f) vrms = 0.0f;

    out[0] = SensorReading::make(0, _id, getType(), "voltage_vrms", vrms,             "V");
    out[1] = SensorReading::make(0, _id, getType(), "voltage_raw",  (float)rmsCount,  "");
    return 2;
}
