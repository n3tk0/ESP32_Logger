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
    if (!_ready || maxOut < 4) return 0;

    // performReading() triggers a new forced-mode reading and waits for it
    if (!_bme.performReading()) return 0;

    float t = _calTemp.apply(_bme.temperature);
    float h = _calHumidity.apply(_bme.humidity);
    float p = _calPressure.apply(_bme.pressure / 100.0f);
    float g = _calGas.apply((float)_bme.gas_resistance);

    out[0] = SensorReading::make(0, _id, getType(), "temperature",    t, "C");
    out[1] = SensorReading::make(0, _id, getType(), "humidity",       h, "%");
    out[2] = SensorReading::make(0, _id, getType(), "pressure",       p, "hPa");
    out[3] = SensorReading::make(0, _id, getType(), "gas_resistance", g, "Ohm");
    return 4;
}
