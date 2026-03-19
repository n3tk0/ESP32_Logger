#include "SCD4xSensor.h"

// CRC-8 per Sensirion: poly=0x31, init=0xFF
uint8_t SCD4xSensor::_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

bool SCD4xSensor::_sendCmd(uint16_t cmd) {
    Wire.beginTransmission(ADDR);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    return Wire.endTransmission() == 0;
}

bool SCD4xSensor::_readWords(uint16_t* words, int count) {
    Wire.requestFrom((int)ADDR, count * 3);
    for (int i = 0; i < count; i++) {
        if (Wire.available() < 3) return false;
        uint8_t msb = Wire.read();
        uint8_t lsb = Wire.read();
        uint8_t crc = Wire.read();
        uint8_t buf[2] = { msb, lsb };
        if (_crc8(buf, 2) != crc) return false;
        words[i] = ((uint16_t)msb << 8) | lsb;
    }
    return true;
}

bool SCD4xSensor::_dataReady() {
    if (!_sendCmd(CMD_GET_DATA_READY_STATUS)) return false;
    delay(1);
    uint16_t status;
    if (!_readWords(&status, 1)) return false;
    // Bits [10:0]: if lower 11 bits != 0, data is ready
    return (status & 0x07FF) != 0;
}

bool SCD4xSensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 5000;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calCo2.load(cal, "co2");
    _calTemp.load(cal, "temperature");
    _calHumidity.load(cal, "humidity");

    // Stop any running measurement first (in case of reset)
    _sendCmd(CMD_STOP_PERIODIC);
    delay(500);

    // Start periodic measurement (5s interval, fixed in SCD40/41)
    if (!_sendCmd(CMD_START_PERIODIC)) {
        Serial.println("[SCD4x] Not found at 0x62");
        return false;
    }

    // Wait for first measurement (SCD40 needs ~5s)
    delay(5100);
    _ready = true;
    Serial.println("[SCD4x] Ready");
    return true;
}

bool SCD4xSensor::read(SensorReading& out) {
    SensorReading buf[3];
    if (readAll(buf, 3) < 1) return false;
    out = buf[0]; // CO2
    return true;
}

int SCD4xSensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 3) return 0;

    if (!_dataReady()) return 0;

    if (!_sendCmd(CMD_READ_MEASUREMENT)) return 0;
    delay(1);

    uint16_t words[3];
    if (!_readWords(words, 3)) return 0;

    // CO2: raw uint16 in ppm
    float co2  = _calCo2.apply((float)words[0]);

    // Temperature: T = -45 + 175 * word / 2^16
    float temp = _calTemp.apply(-45.0f + 175.0f * (float)words[1] / 65536.0f);

    // Humidity: RH = 100 * word / 2^16
    float rh   = _calHumidity.apply(100.0f * (float)words[2] / 65536.0f);

    out[0] = SensorReading::make(0, _id, getType(), "co2",         co2,  "ppm");
    out[1] = SensorReading::make(0, _id, getType(), "temperature", temp, "C");
    out[2] = SensorReading::make(0, _id, getType(), "humidity",    rh,   "%");
    _lastReadTs = 0;
    return 3;
}
