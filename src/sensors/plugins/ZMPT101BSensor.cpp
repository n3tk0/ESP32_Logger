#include "ZMPT101BSensor.h"
#include <math.h>

bool ZMPT101BSensor::init(JsonObjectConst cfg) {
    _enabled        = cfg["enabled"]          | true;
    _pin            = cfg["pin"]              | 0;
    _voltageFactor  = cfg["voltage_factor"]   | 1.0f;
    _samples        = cfg["adc_samples"]      | 200;
    _samplePeriodUs = cfg["sample_period_us"] | 100;
    _intervalMs     = cfg["read_interval_ms"] | 1000;

    if (_samples < 10)  _samples = 10;
    if (_samples > 500) _samples = 500;

    JsonObjectConst cal = cfg["calibration"];
    _calVoltage.load(cal, "voltage_vrms");

    analogSetPinAttenuation(_pin, ADC_11db); // full-scale ~3.3 V
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
    float vrms = (float)(rmsCount * _voltageFactor);
    vrms = _calVoltage.apply(vrms);
    if (vrms < 0.0f) vrms = 0.0f;

    out[0] = SensorReading::make(0, _id, getType(), "voltage_vrms", vrms,             "V");
    out[1] = SensorReading::make(0, _id, getType(), "voltage_raw",  (float)rmsCount,  "");
    return 2;
}
