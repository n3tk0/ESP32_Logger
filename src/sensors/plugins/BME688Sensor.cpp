#include "BME688Sensor.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"        // R17: _claim/_release helpers

bool BME688Sensor::init(JsonObjectConst cfg) {
    _enabled      = cfg["enabled"]            | true;
    _intervalMs   = cfg["read_interval_ms"]   | 10000;
    _addr         = (uint8_t)(cfg["address"]  | 0x76);
    _heaterTemp   = cfg["heater_temp"]        | 320;
    _heaterDurMs  = cfg["heater_duration_ms"] | 150;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (!validateAttachPin(sda, "bme688", "sda")) return false;
    if (!validateAttachPin(scl, "bme688", "scl")) return false;
    if (!_claimI2cAddress(_addr, this)) {
        Serial.printf("[BME688] I2C address 0x%02X already claimed — refusing init\n", _addr);
        return false;
    }
    Wire.begin((int8_t)sda, (int8_t)scl);

    JsonObjectConst cal = cfg["calibration"];
    _calTemp.load(cal, "temperature");
    _calHumidity.load(cal, "humidity");
    _calPressure.load(cal, "pressure");
    _calGas.load(cal, "gas_resistance");

    if (!_bme.begin(_addr, &Wire)) {
        Serial.printf("[BME688] Not found at 0x%02X\n", _addr);
        return false;
    }

    // Configure sensor oversampling and filter
    _bme.setTemperatureOversampling(BME688_Mini::OS_8X);
    _bme.setHumidityOversampling(BME688_Mini::OS_2X);
    _bme.setPressureOversampling(BME688_Mini::OS_4X);
    _bme.setIIRFilterSize(BME688_Mini::FILTER_3);
    _bme.setGasHeater(_heaterTemp, _heaterDurMs);

    _ready = true;
    Serial.printf("[BME688] Ready at 0x%02X heater=%d°C/%dms\n",
                  _addr, _heaterTemp, _heaterDurMs);
    return true;
}

bool BME688Sensor::read(SensorReading& out) {
    SensorReading buf[4];
    if (readAll(buf, 4) < 1) return false;
    out = buf[0];
    return true;
}

int BME688Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;

    // performReading() triggers a new forced-mode reading and waits for it
    if (!_bme.performReading()) return 0;

    float rawGas = (float)_bme.gas_resistance;          // Ω, uncalibrated
    float t   = _calTemp.apply(_bme.temperature);
    float h   = _calHumidity.apply(_bme.humidity);
    float p   = _calPressure.apply(_bme.pressure / 100.0f);
    float g   = _calGas.apply(rawGas);
    float iaq = _computeIaq(h, rawGas);                 // 0..500 (lower = cleaner)

    int n = (maxOut < 5) ? maxOut : 5;
    if (n > 0) out[0] = SensorReading::make(0, _id, getType(), "temperature",    t,   "C");
    if (n > 1) out[1] = SensorReading::make(0, _id, getType(), "humidity",       h,   "%");
    if (n > 2) out[2] = SensorReading::make(0, _id, getType(), "pressure",       p,   "hPa");
    if (n > 3) out[3] = SensorReading::make(0, _id, getType(), "gas_resistance", g,   "Ohm");
    if (n > 4) out[4] = SensorReading::make(0, _id, getType(), "iaq",            iaq, "");
    return n;
}

// IAQ (0..500, lower = cleaner; BSEC convention) — humidity + gas heuristic.
// No Bosch BSEC: a self-calibrating clean-air baseline tracks the upper
// envelope of the MOX resistance, and the index combines a humidity score
// (optimal RH ≈ 40 %) with the current resistance ratio to that baseline.
float BME688Sensor::_computeIaq(float humidity, float rawGasOhm) {
    // Baseline: rise quickly toward a higher (cleaner) resistance ceiling,
    // drift down very slowly to absorb sensor aging / ambient drift.
    if (_gasBaseline <= 0.0f)          _gasBaseline = rawGasOhm;                          // seed
    else if (rawGasOhm > _gasBaseline) _gasBaseline += (rawGasOhm - _gasBaseline) * 0.10f;
    else                               _gasBaseline += (rawGasOhm - _gasBaseline) * 0.0005f;
    if (_gasBaseline < 1.0f) _gasBaseline = 1.0f;

    // Humidity contribution (0..25): peaks in the 38–42 % comfort band.
    float humScore;
    if (humidity >= 38.0f && humidity <= 42.0f) humScore = 25.0f;
    else if (humidity < 38.0f)                  humScore = (humidity / 38.0f) * 25.0f;
    else                                        humScore = ((100.0f - humidity) / 58.0f) * 25.0f;
    if (humScore < 0.0f)  humScore = 0.0f;
    if (humScore > 25.0f) humScore = 25.0f;

    // Gas contribution (0..75): current resistance relative to clean baseline.
    float ratio = rawGasOhm / _gasBaseline;   // ≈1 when clean, <1 when polluted
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.0f) ratio = 0.0f;
    float gasScore = ratio * 75.0f;

    // Quality 0..100 (higher = cleaner) inverted to IAQ 0..500 (lower = cleaner).
    float iaq = (100.0f - (humScore + gasScore)) * 5.0f;
    if (iaq < 0.0f)   iaq = 0.0f;
    if (iaq > 500.0f) iaq = 500.0f;
    return iaq;
}
