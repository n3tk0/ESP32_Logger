#include "SPS30Sensor.h"
#include <Wire.h>
#include <string.h>
#include <math.h>
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"           // R17: _claimI2cAddress

// CRC-8 per Sensirion: poly=0x31, init=0xFF (same as SCD4x).
uint8_t SPS30Sensor::_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

bool SPS30Sensor::_sendCmd(uint16_t cmd) {
    Wire.beginTransmission(ADDR);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    return Wire.endTransmission() == 0;
}

// Command + one 16-bit argument word, each followed by its CRC (used to start
// measurement with the float output format).
bool SPS30Sensor::_sendCmdArg(uint16_t cmd, uint16_t arg) {
    uint8_t a[2] = { (uint8_t)(arg >> 8), (uint8_t)(arg & 0xFF) };
    Wire.beginTransmission(ADDR);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    Wire.write(a[0]);
    Wire.write(a[1]);
    Wire.write(_crc8(a, 2));
    return Wire.endTransmission() == 0;
}

bool SPS30Sensor::_readWords(uint16_t* words, int count) {
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

bool SPS30Sensor::_dataReady() {
    if (!_sendCmd(CMD_READ_DATA_READY)) return false;
    delay(1);
    uint16_t status;
    if (!_readWords(&status, 1)) return false;
    return (status & 0x0001) != 0;   // LSB set → a new measurement is ready
}

bool SPS30Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 5000;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (!validateAttachPin(sda, "sps30", "sda")) return false;
    if (!validateAttachPin(scl, "sps30", "scl")) return false;
    if (!_claimI2cAddress(ADDR, this)) {
        Serial.printf("[SPS30] I2C address 0x%02X already claimed — refusing init\n", ADDR);
        return false;
    }
    Wire.begin((int8_t)sda, (int8_t)scl);

    JsonObjectConst cal = cfg["calibration"];
    _calPm1.load(cal,  "pm1");
    _calPm25.load(cal, "pm25");
    _calPm4.load(cal,  "pm4");
    _calPm10.load(cal, "pm10");

    // Wake the sensor in case a previous session left it asleep (while idle the
    // wake-up command is simply NACKed — harmless), then stop any running
    // measurement for a clean state.
    _sendCmd(CMD_WAKE);
    delay(5);
    _sendCmd(CMD_STOP_MEASURE);
    delay(20);

    // Start measurement in IEEE-754 float output format.
    if (!_sendCmdArg(CMD_START_MEASURE, ARG_FLOAT_FORMAT)) {
        Serial.println("[SPS30] Not found / no ACK at 0x69");
        return false;
    }

    // Defer the fan spin-up out of init(); readAll() gates on _warmupUntilMs.
    // SPS30 yields a new sample every ~1 s, but the first few seconds are
    // unstable until the fan reaches speed.
    _warmupUntilMs = millis() + 8000;
    _ready = true;
    Serial.println("[SPS30] Started — first reading in ~8s");
    return true;
}

bool SPS30Sensor::read(SensorReading& out) {
    SensorReading buf[4];
    if (readAll(buf, 4) < 1) return false;
    out = buf[1];   // PM2.5 — the most-used single metric
    return true;
}

int SPS30Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 4) return 0;
    if ((int32_t)(millis() - _warmupUntilMs) < 0) return 0;   // still warming up

    if (!_dataReady()) return 0;

    if (!_sendCmd(CMD_READ_MEASURED)) return 0;
    delay(1);

    // Float output format streams 10 floats; the four mass concentrations
    // (PM1.0, PM2.5, PM4.0, PM10) come first. Each float is two CRC-checked
    // 16-bit words, so read the first 8 words and drop the rest of the frame.
    uint16_t w[8];
    if (!_readWords(w, 8)) return 0;

    auto toFloat = [](uint16_t hi, uint16_t lo) -> float {
        uint32_t bits = ((uint32_t)hi << 16) | lo;   // big-endian IEEE-754 word
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    };
    float pm1  = toFloat(w[0], w[1]);
    float pm25 = toFloat(w[2], w[3]);
    float pm4  = toFloat(w[4], w[5]);
    float pm10 = toFloat(w[6], w[7]);

    // Reject NaN/inf or negative values — a CRC-passed-but-corrupt/stale frame.
    if (!isfinite(pm1) || !isfinite(pm25) || !isfinite(pm4) || !isfinite(pm10)) return 0;
    if (pm1 < 0.0f || pm25 < 0.0f || pm4 < 0.0f || pm10 < 0.0f) return 0;

    out[0] = SensorReading::make(0, _id, getType(), "pm1",  _calPm1.apply(pm1),   "ug/m3");
    out[1] = SensorReading::make(0, _id, getType(), "pm25", _calPm25.apply(pm25), "ug/m3");
    out[2] = SensorReading::make(0, _id, getType(), "pm4",  _calPm4.apply(pm4),   "ug/m3");
    out[3] = SensorReading::make(0, _id, getType(), "pm10", _calPm10.apply(pm10), "ug/m3");
    return 4;
}
