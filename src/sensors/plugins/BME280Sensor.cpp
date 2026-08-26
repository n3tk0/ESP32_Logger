#include "BME280Sensor.h"
#include "../I2CBus.h"
#include "../ReadingCache.h"             // ambient temperature reference
#include "../../utils/Psychrometrics.h"  // dew point / RH re-expression
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"        // R17: _claim/_release helpers

// ---------------------------------------------------------------------------
bool BME280Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"] | true;
    _intervalMs = cfg["read_interval_ms"] | 10000;
    _addr       = (uint8_t)(cfg["address"] | 0x76);

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    // I2C bus selection. acquire() validates both pins against the board
    // profile, rejects a bus this chip does not have, and refuses a bus
    // already brought up on different pins.
    _bus  = (uint8_t)(cfg["bus"] | 0);
    _wire = I2CBus::acquire(_bus, sda, scl, "bme280");
    if (!_wire) return false;
    if (!_claimI2cAddress(_bus, _addr, this)) {
        Serial.printf("[BME280] I2C address 0x%02X already claimed on bus %u — refusing init\n", _addr, (unsigned)_bus);
        return false;
    }

    _ready = _bme.begin(_addr, _wire);
    if (!_ready) {
        Serial.printf("[BME280] Not found at 0x%02X\n", _addr);
        return false;
    }

    // BME280_Mini auto-detects chip type via chip ID register
    _isBMP280 = !_bme.isBME280();
    Serial.printf("[BME280] chip_id=0x%02X → %s\n",
                  _bme.chipId(), _isBMP280 ? "BMP280" : "BME280");

    // Load calibration
    JsonObjectConst cal = cfg["calibration"];
    _calTemp.load(cal, "temperature");
    _calHumidity.load(cal, "humidity");
    _calPressure.load(cal, "pressure");

    const char* ambSensor = cfg["ambient_temp_sensor"] | "";
    strlcpy(_ambientSensor, ambSensor, sizeof(_ambientSensor));
    const char* ambMetric = cfg["ambient_temp_metric"] | "temperature";
    strlcpy(_ambientMetric, ambMetric, sizeof(_ambientMetric));
    _ambientMaxAgeMs = cfg["ambient_max_age_ms"] | 60000;

    Serial.printf("[%s] ready at 0x%02X  cal_T(%.2f+%.2fx) cal_P(%.2f+%.2fx) ambient_ref=%s\n",
                  getType(), _addr,
                  _calTemp.offset, _calTemp.scale,
                  _calPressure.offset, _calPressure.scale,
                  _ambientSensor[0] ? _ambientSensor : "(self)");
    return true;
}

// ---------------------------------------------------------------------------
// Air temperature to express the humidity against. Falls back to `fallbackC`
// (this sensor's own calibrated temperature) whenever the configured reference
// is missing, stale or implausible — a frozen last-known value would be worse
// than an admittedly self-heated one, because it looks equally healthy.
//
// Lifted from BME688Sensor with the sensor name changed. The duplication is
// deliberate: folding it into a shared helper would put a ReadingCache lookup
// and a warning-latch in ISensor for the benefit of two plugins, and the next
// sensor that wants it will want a different fallback.
float BME280Sensor::_ambientTempC(float fallbackC) const {
    if (_ambientSensor[0] == '\0') return fallbackC;

    float    refC  = 0.0f;
    uint32_t ageMs = 0;
    if (!readingCache.get(_ambientSensor, _ambientMetric, refC, ageMs)) {
        if (!_ambientWarned) {
            Serial.printf("[%s] ambient ref '%s/%s' not seen yet — using own temperature\n",
                          getType(), _ambientSensor, _ambientMetric);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (ageMs > _ambientMaxAgeMs) {
        if (!_ambientWarned) {
            Serial.printf("[%s] ambient ref '%s/%s' stale (%lums) — using own temperature\n",
                          getType(), _ambientSensor, _ambientMetric, (unsigned long)ageMs);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (!Psychro::isValidTemp(refC)) return fallbackC;

    _ambientWarned = false;   // arm the next warning
    return refC;
}

// ---------------------------------------------------------------------------
bool BME280Sensor::read(SensorReading& out) {
    if (!_ready) return false;
    float t = _calTemp.apply(_bme.readTemperature());
    if (isnan(t)) return false;
    out = _makeReading(0, "temperature", t, "C");
    return true;
}

// ---------------------------------------------------------------------------
int BME280Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready) return 0;

    const float tDie = _bme.readTemperature();       // uncalibrated, self-heated
    float t = _calTemp.apply(tDie);
    float p = _calPressure.apply(_bme.readPressure() / 100.0f);

    if (isnan(t) || isnan(p)) return 0;

    if (_isBMP280) {
        if (maxOut < 2) return 0;
        out[0] = _makeReading(0, "temperature", t, "C");
        out[1] = _makeReading(0, "pressure",    p, "hPa");
        return 2;
    }

    if (maxOut < 3) return 0;
    float h = _calHumidity.apply(_bme.readHumidity());
    if (isnan(h)) return 0;

    // Dew point pairs the RH with the temperature it was measured AT — the raw
    // die temperature, not the calibrated one. Pairing it with a corrected
    // temperature would bake the self-heating error into the dew point, which
    // is the one figure that is supposed to be free of it.
    const float dew  = Psychro::dewPointC(tDie, h);
    const float hAmb = isfinite(dew) ? Psychro::rhAtTempC(dew, _ambientTempC(t)) : NAN;

    int n = 0;
    out[n++] = _makeReading(0, "temperature", t, "C");
    out[n++] = _makeReading(0, "humidity",    h, "%");
    out[n++] = _makeReading(0, "pressure",    p, "hPa");
    // Dropped rather than emitted as NaN when the inputs are out of range: a
    // NaN would land in storage as a permanent null.
    if (isfinite(dew)  && n < maxOut) out[n++] = _makeReading(0, "dew_point",        dew,  "C");
    if (isfinite(hAmb) && n < maxOut) out[n++] = _makeReading(0, "humidity_ambient", hAmb, "%");
    return n;
}

// ---------------------------------------------------------------------------
SensorReading BME280Sensor::_makeReading(uint32_t ts, const char* metric,
                                         float value, const char* unit) const
{
    return SensorReading::make(ts, _id, getType(), metric, value, unit);
}
