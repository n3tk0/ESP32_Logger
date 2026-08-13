#include "BME688Sensor.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"        // R17: _claim/_release helpers
#include "../ReadingCache.h"         // ambient temperature reference
#include "../../utils/Psychrometrics.h"

bool BME688Sensor::init(JsonObjectConst cfg) {
    _enabled      = cfg["enabled"]            | true;
    _intervalMs   = cfg["read_interval_ms"]   | 10000;
    _addr         = (uint8_t)(cfg["address"]  | 0x76);
    _heaterTemp   = cfg["heater_temp"]        | 320;
    _heaterDurMs  = cfg["heater_duration_ms"] | 150;

    // Ambient temperature reference for the humidity correction.
    const char* ambSensor = cfg["ambient_temp_sensor"] | "";
    strlcpy(_ambientSensor, ambSensor, sizeof(_ambientSensor));
    const char* ambMetric = cfg["ambient_temp_metric"] | "temperature";
    strlcpy(_ambientMetric, ambMetric, sizeof(_ambientMetric));
    _ambientMaxAgeMs = cfg["ambient_max_age_ms"] | 60000;
    _ambientWarned   = false;

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
    Serial.printf("[BME688] Ready at 0x%02X heater=%d°C/%dms ambient_ref=%s\n",
                  _addr, _heaterTemp, _heaterDurMs,
                  _ambientSensor[0] ? _ambientSensor : "(self)");
    return true;
}

bool BME688Sensor::read(SensorReading& out) {
    SensorReading buf[7];
    if (readAll(buf, 7) < 1) return false;
    out = buf[0];
    return true;
}

// ---------------------------------------------------------------------------
// Air temperature to express the humidity against. Falls back to `fallbackC`
// (this sensor's own calibrated temperature) whenever the configured reference
// is missing, stale or implausible — a frozen last-known value would be worse
// than an admittedly self-heated one, because it looks equally healthy.
float BME688Sensor::_ambientTempC(float fallbackC) const {
    if (_ambientSensor[0] == '\0') return fallbackC;

    float    refC  = 0.0f;
    uint32_t ageMs = 0;
    if (!readingCache.get(_ambientSensor, _ambientMetric, refC, ageMs)) {
        if (!_ambientWarned) {
            Serial.printf("[BME688] ambient ref '%s/%s' not seen yet — using own temperature\n",
                          _ambientSensor, _ambientMetric);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (ageMs > _ambientMaxAgeMs) {
        if (!_ambientWarned) {
            Serial.printf("[BME688] ambient ref '%s/%s' stale (%lums) — using own temperature\n",
                          _ambientSensor, _ambientMetric, (unsigned long)ageMs);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (!Psychro::isValidTemp(refC)) return fallbackC;

    _ambientWarned = false;   // arm the next warning
    return refC;
}

int BME688Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;

    // performReading() triggers a new forced-mode reading and waits for it
    if (!_bme.performReading()) return 0;

    float rawGas = (float)_bme.gas_resistance;          // Ω, uncalibrated
    float tDie = _bme.temperature;                      // die temperature, uncalibrated
    float t   = _calTemp.apply(tDie);
    float h   = _calHumidity.apply(_bme.humidity);
    float p   = _calPressure.apply(_bme.pressure / 100.0f);
    float g   = _calGas.apply(rawGas);
    float iaq = _computeIaq(h, rawGas);                 // 0..500 (lower = cleaner)

    // Dew point pairs the RH with the temperature it was measured AT — the raw
    // die temperature, not the calibrated one. Pairing it with a corrected
    // temperature would bake the self-heating error into the dew point, which
    // is the one figure that is supposed to be free of it.
    float dew  = Psychro::dewPointC(tDie, h);
    float hAmb = isfinite(dew) ? Psychro::rhAtTempC(dew, _ambientTempC(t)) : NAN;

    int n = 0;
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "temperature",    t,   "C");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "humidity",       h,   "%");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "pressure",       p,   "hPa");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "gas_resistance", g,   "Ohm");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "iaq",            iaq, "");
    // Derived metrics are dropped rather than emitted as NaN when the inputs
    // are out of range: a NaN would land in storage as a permanent null.
    if (isfinite(dew)  && n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "dew_point",        dew,  "C");
    if (isfinite(hAmb) && n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "humidity_ambient", hAmb, "%");
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
