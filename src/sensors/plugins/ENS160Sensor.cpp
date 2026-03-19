#include "ENS160Sensor.h"

bool ENS160Sensor::_writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool ENS160Sensor::_readRegs(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)_addr, (int)len);
    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

bool ENS160Sensor::_waitReady(uint32_t timeoutMs) {
    unsigned long t = millis();
    while (millis() - t < timeoutMs) {
        uint8_t status = 0;
        if (_readRegs(REG_STATUS, &status, 1) && (status & 0x08)) return true;
        delay(50);
    }
    return false;
}

bool ENS160Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 30000;
    _addr       = (uint8_t)(cfg["address"]| 0x52);

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calTvoc.load(cal, "tvoc");
    _calEco2.load(cal, "eco2");

    // Set standard operating mode
    if (!_writeReg(REG_OPMODE, MODE_STANDARD)) {
        Serial.printf("[ENS160] Not found at 0x%02X\n", _addr);
        return false;
    }
    delay(100);
    _ready = _waitReady(2000);
    if (!_ready) Serial.println("[ENS160] Not ready after init");
    return _ready;
}

bool ENS160Sensor::read(SensorReading& out) {
    SensorReading buf[3];
    if (readAll(buf, 3) < 1) return false;
    out = buf[0]; // TVOC
    return true;
}

int ENS160Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 3) return 0;

    uint8_t aqi = 0;
    uint8_t tvocBuf[2], eco2Buf[2];

    if (!_readRegs(REG_AQI,    &aqi,      1)) return 0;
    if (!_readRegs(REG_TVOC_L, tvocBuf,   2)) return 0;
    if (!_readRegs(REG_ECO2_L, eco2Buf,   2)) return 0;

    float tvoc = _calTvoc.apply((float)((tvocBuf[1] << 8) | tvocBuf[0]));
    float eco2 = _calEco2.apply((float)((eco2Buf[1] << 8) | eco2Buf[0]));

    out[0] = SensorReading::make(0, _id, getType(), "tvoc",  tvoc,       "ppb");
    out[1] = SensorReading::make(0, _id, getType(), "eco2",  eco2,       "ppm");
    out[2] = SensorReading::make(0, _id, getType(), "aqi",   (float)aqi, "");
    _lastReadTs = 0;
    return 3;
}
