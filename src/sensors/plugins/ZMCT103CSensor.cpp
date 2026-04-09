#include "ZMCT103CSensor.h"
#include <math.h>

bool ZMCT103CSensor::init(JsonObjectConst cfg) {
    _enabled        = cfg["enabled"]          | true;
    _pin            = cfg["pin"]              | 1;
    _currentFactor  = cfg["current_factor"]   | 1.0f;
    _samples        = cfg["adc_samples"]      | 200;
    _samplePeriodUs = cfg["sample_period_us"] | 100;
    _intervalMs     = cfg["read_interval_ms"] | 1000;

    if (_samples < 10)  _samples = 10;
    if (_samples > 500) _samples = 500;

    JsonObjectConst cal = cfg["calibration"];
    _calCurrent.load(cal, "current_arms");

    analogSetPinAttenuation(_pin, ADC_11db); // full-scale ~3.3 V
    pinMode(_pin, INPUT);
    _ready = true;

    Serial.printf("[ZMCT103C] pin=%d factor=%.6f samples=%d period_us=%u\n",
                  _pin, _currentFactor, _samples, _samplePeriodUs);
    return true;
}

bool ZMCT103CSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int ZMCT103CSensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 2) return 0;

    // -----------------------------------------------------------------------
    // Zero-DC RMS estimation (identical algorithm to ZMPT101B):
    //   1. Collect N samples with fixed inter-sample delay.
    //   2. Compute mean (DC bias ≈ VCC/2).
    //   3. Subtract mean, compute variance → √ → AC RMS count.
    //   4. Multiply by current_factor → Arms.
    // -----------------------------------------------------------------------
    double sum = 0.0;
    int samples = _samples;
    int* buf = (int*)alloca(sizeof(int) * samples);

    for (int i = 0; i < samples; i++) {
        buf[i] = analogRead(_pin);
        sum += buf[i];
        delayMicroseconds(_samplePeriodUs);
    }
    double mean = sum / samples;

    double sumSq = 0.0;
    for (int i = 0; i < samples; i++) {
        double diff = buf[i] - mean;
        sumSq += diff * diff;
    }
    double rmsCount = sqrt(sumSq / samples);
    float arms = (float)(rmsCount * _currentFactor);
    arms = _calCurrent.apply(arms);
    if (arms < 0.0f) arms = 0.0f;

    out[0] = SensorReading::make(0, _id, getType(), "current_arms", arms,             "A");
    out[1] = SensorReading::make(0, _id, getType(), "current_raw",  (float)rmsCount,  "");
    return 2;
}
