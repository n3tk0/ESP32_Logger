#include "BH1750Sensor.h"

bool BH1750Sensor::_sendCmd(uint8_t cmd) {
    Wire.beginTransmission(_addr);
    Wire.write(cmd);
    return Wire.endTransmission() == 0;
}

bool BH1750Sensor::_readLux(float& lux) {
    Wire.requestFrom((int)_addr, 2);
    if (Wire.available() < 2) return false;
    uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
    lux = _calLux.apply((float)raw / _divider);
    return true;
}

bool BH1750Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 2000;
    _addr       = (uint8_t)(cfg["address"] | 0x23);

    const char* mode = cfg["mode"] | "H";
    if (strcmp(mode, "H2") == 0) {
        _modeCmd  = CMD_CONT_H2;
        _divider  = 2.4f; // 0.5 lx resolution
    } else if (strcmp(mode, "L") == 0) {
        _modeCmd  = CMD_CONT_L;
        _divider  = 0.3f; // 4 lx resolution (~1.2/4)
    } else {
        _modeCmd  = CMD_CONT_H;
        _divider  = 1.2f;
    }

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calLux.load(cal, "lux");

    if (!_sendCmd(CMD_POWER_ON)) {
        Serial.printf("[BH1750] Not found at 0x%02X\n", _addr);
        return false;
    }
    delay(10);
    _sendCmd(CMD_RESET);
    delay(10);
    _sendCmd(_modeCmd);
    delay(180); // wait for first measurement (120ms for H mode, some margin)

    _ready = true;
    Serial.printf("[BH1750] Ready at 0x%02X mode=%s\n", _addr, mode);
    return true;
}

bool BH1750Sensor::read(SensorReading& out) {
    SensorReading buf[1];
    if (readAll(buf, 1) < 1) return false;
    out = buf[0];
    return true;
}

int BH1750Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;
    float lux;
    if (!_readLux(lux)) return 0;
    out[0] = SensorReading::make(0, _id, getType(), "lux", lux, "lx");
    return 1;
}
