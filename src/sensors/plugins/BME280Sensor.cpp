#include "BME280Sensor.h"
#include "../I2CBus.h"
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

    Serial.printf("[%s] ready at 0x%02X  cal_T(%.2f+%.2fx) cal_P(%.2f+%.2fx)\n",
                  getType(), _addr,
                  _calTemp.offset, _calTemp.scale,
                  _calPressure.offset, _calPressure.scale);
    return true;
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

    float t = _calTemp.apply(_bme.readTemperature());
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

    out[0] = _makeReading(0, "temperature", t, "C");
    out[1] = _makeReading(0, "humidity",    h, "%");
    out[2] = _makeReading(0, "pressure",    p, "hPa");
    return 3;
}

// ---------------------------------------------------------------------------
SensorReading BME280Sensor::_makeReading(uint32_t ts, const char* metric,
                                         float value, const char* unit) const
{
    return SensorReading::make(ts, _id, getType(), metric, value, unit);
}
