#include "BME280Sensor.h"

// ---------------------------------------------------------------------------
bool BME280Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"] | true;
    _intervalMs = cfg["read_interval_ms"] | 10000;
    _addr       = (uint8_t)(cfg["address"] | 0x76);

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;

    // Allow custom I2C pins; fall back to default Wire
    if (sda >= 0 && scl >= 0) {
        Wire.begin((int8_t)sda, (int8_t)scl);
    } else {
        Wire.begin();
    }

    _ready = _bme.begin(_addr, &Wire);
    if (!_ready) {
        Serial.printf("[BME280] Not found at 0x%02X\n", _addr);
    }
    return _ready;
}

// ---------------------------------------------------------------------------
bool BME280Sensor::read(SensorReading& out) {
    if (!_ready) return false;
    float t = _bme.readTemperature();
    if (isnan(t)) return false;
    out = _makeReading(0, "temperature", t, "C");
    return true;
}

// ---------------------------------------------------------------------------
int BME280Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 3) return 0;

    float t = _bme.readTemperature();
    float h = _bme.readHumidity();
    float p = _bme.readPressure() / 100.0f; // Pa → hPa

    if (isnan(t) || isnan(h) || isnan(p)) return 0;

    out[0] = _makeReading(0, "temperature", t, "C");
    out[1] = _makeReading(0, "humidity",    h, "%");
    out[2] = _makeReading(0, "pressure",    p, "hPa");

    _lastReadTs = 0; // Set by SensorManager from RTC/NTP
    return 3;
}

// ---------------------------------------------------------------------------
SensorReading BME280Sensor::_makeReading(uint32_t ts, const char* metric,
                                         float value, const char* unit) const
{
    return SensorReading::make(ts, _id, getType(), metric, value, unit);
}
