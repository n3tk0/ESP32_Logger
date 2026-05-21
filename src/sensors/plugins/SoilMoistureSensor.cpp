#include "SoilMoistureSensor.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin

// R28 / AUDIT 24.11: see ZMPT101BSensor.cpp — IDF 5.x renamed ADC_11db.
#ifndef ADC_ATTEN_DB_12
#  define ADC_ATTEN_DB_12 ADC_11db
#endif

bool SoilMoistureSensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _pin        = cfg["pin"]              | -1;  // R11: unset → init refuses
    _dryValue   = cfg["dry_value"]        | 3300;
    _wetValue   = cfg["wet_value"]        | 1500;
    _samples    = cfg["adc_samples"]      | 16;
    _intervalMs = cfg["read_interval_ms"] | 5000;

    if (_samples < 1) _samples = 1;
    if (_samples > 64) _samples = 64;

    // Ensure dry_value > wet_value (sensor is inverted)
    if (_dryValue <= _wetValue) {
        int tmp  = _dryValue;
        _dryValue = _wetValue;
        _wetValue = tmp;
    }

    JsonObjectConst cal = cfg["calibration"];
    _calMoisture.load(cal, "moisture");

    if (!validateAttachPin(_pin, "soil_moisture", "pin")) return false;
    // R28 / AUDIT 24.5: capacitive soil sensors swing up to ~3.0 V. Default
    // ADC attenuation on ESP32-C3 caps the readable range near 1.5 V, so the
    // "dry air" end clips. ADC_ATTEN_DB_12 widens the range to ~3.1 V before
    // clipping (legacy alias ADC_11db; see 24.11).
    analogSetPinAttenuation(_pin, ADC_ATTEN_DB_12);
    pinMode(_pin, INPUT);
    _ready = true;
    Serial.printf("[SoilMoisture] pin=%d dry=%d wet=%d samples=%d\n",
                  _pin, _dryValue, _wetValue, _samples);
    return true;
}

bool SoilMoistureSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int SoilMoistureSensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 2) return 0;

    // Average multiple ADC readings to reduce noise
    long sum = 0;
    for (int i = 0; i < _samples; i++) {
        sum += analogRead(_pin);
        delayMicroseconds(200);
    }
    int raw = (int)(sum / _samples);

    // Map: dry(high) → 0%, wet(low) → 100%
    // constrain then map
    int clamped = constrain(raw, _wetValue, _dryValue);
    float pct = (float)(_dryValue - clamped) / (float)(_dryValue - _wetValue) * 100.0f;
    pct = _calMoisture.apply(pct);
    pct = constrain(pct, 0.0f, 100.0f);

    out[0] = SensorReading::make(0, _id, getType(), "moisture_pct", pct,       "%");
    out[1] = SensorReading::make(0, _id, getType(), "moisture_raw", (float)raw, "");
    return 2;
}
