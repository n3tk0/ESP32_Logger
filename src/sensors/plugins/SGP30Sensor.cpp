#include "SGP30Sensor.h"

// CRC-8 (polynomial 0x31, init 0xFF) per Sensirion datasheet
uint8_t SGP30Sensor::_crc8(uint8_t d1, uint8_t d2) {
    uint8_t crc = 0xFF;
    crc ^= d1;
    for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    crc ^= d2;
    for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    return crc;
}

bool SGP30Sensor::_sendCommand(uint16_t cmd) {
    Wire.beginTransmission(ADDR);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    return Wire.endTransmission() == 0;
}

bool SGP30Sensor::_readWords(uint16_t* words, int count) {
    Wire.requestFrom((int)ADDR, count * 3);
    for (int i = 0; i < count; i++) {
        if (Wire.available() < 3) return false;
        uint8_t msb = Wire.read();
        uint8_t lsb = Wire.read();
        uint8_t crc = Wire.read();
        if (crc != _crc8(msb, lsb)) return false;
        words[i] = ((uint16_t)msb << 8) | lsb;
    }
    return true;
}

bool SGP30Sensor::_measure(uint16_t& tvoc, uint16_t& eco2) {
    if (!_sendCommand(CMD_IAQ_MEAS)) return false;
    delay(12); // SGP30 measurement takes 12ms
    uint16_t words[2];
    if (!_readWords(words, 2)) return false;
    eco2 = words[0];
    tvoc = words[1];
    return true;
}

bool SGP30Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 30000;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calTvoc.load(cal, "tvoc");
    _calEco2.load(cal, "eco2");

    // Verify chip is present by reading feature set
    if (!_sendCommand(CMD_GET_FEAT)) {
        DBGLN("[SGP30] Not found at 0x58");
        return false;
    }
    delay(10);
    uint16_t feat;
    if (!_readWords(&feat, 1)) {
        DBGLN("[SGP30] Feature read failed");
        return false;
    }

    // Start IAQ algorithm
    if (!_sendCommand(CMD_IAQ_INIT)) {
        DBGLN("[SGP30] IAQ init failed");
        return false;
    }

    _initMs = millis();
    _ready  = true;
    DBGF("[SGP30] Ready (feature=0x%04X)\n", feat);
    return true;
}

bool SGP30Sensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0]; // TVOC
    return true;
}

int SGP30Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 2) return 0;

    // Skip first 15s (warmup period — readings are baseline values, not useful)
    bool warmedUp = (millis() - _initMs) >= 15000;

    uint16_t tvoc, eco2;
    if (!_measure(tvoc, eco2)) return 0;

    SensorQuality q = warmedUp ? QUALITY_GOOD : QUALITY_ESTIMATED;
    out[0] = SensorReading::make(0, _id, getType(), "tvoc", _calTvoc.apply((float)tvoc), "ppb", q);
    out[1] = SensorReading::make(0, _id, getType(), "eco2", _calEco2.apply((float)eco2), "ppm", q);
    return 2;
}
