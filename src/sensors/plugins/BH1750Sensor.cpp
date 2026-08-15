#include "BH1750Sensor.h"
#include "../I2CBus.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"        // R17: _claim/_release helpers

bool BH1750Sensor::_sendCmd(uint8_t cmd) {
    _wire->beginTransmission(_addr);
    _wire->write(cmd);
    return _wire->endTransmission() == 0;
}

bool BH1750Sensor::_readLux(float& lux) {
    _wire->requestFrom((int)_addr, 2);
    if (_wire->available() < 2) return false;
    uint16_t raw = ((uint16_t)_wire->read() << 8) | _wire->read();
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
        _divider  = 1.2f; // L mode: same 1.2 count/lx conversion as H (1 lx resolution)
    } else {
        _modeCmd  = CMD_CONT_H;
        _divider  = 1.2f;
    }

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    // I2C bus selection. acquire() validates both pins against the board
    // profile, rejects a bus this chip does not have, and refuses a bus
    // already brought up on different pins.
    _bus  = (uint8_t)(cfg["bus"] | 0);
    _wire = I2CBus::acquire(_bus, sda, scl, "bh1750");
    if (!_wire) return false;
    if (!_claimI2cAddress(_bus, _addr, this)) {
        Serial.printf("[BH1750] I2C address 0x%02X already claimed on bus %u — refusing init\n", _addr, (unsigned)_bus);
        return false;
    }

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
