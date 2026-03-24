#include "VEML7700Sensor.h"

bool VEML7700Sensor::_writeReg(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write(val >> 8);
    return Wire.endTransmission() == 0;
}

bool VEML7700Sensor::_readReg(uint8_t reg, uint16_t& val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)ADDR, 2);
    if (Wire.available() < 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    val = ((uint16_t)hi << 8) | lo;
    return true;
}

// Resolution table from VEML7700 datasheet (Table 1)
// gain: 0=1x, 1=2x, 2=1/8x, 3=1/4x
// it_ms: 25/50/100/200/400/800
static float _lookupResolution(uint8_t gain, uint16_t itMs) {
    // Base resolution at gain=1x, IT=100ms = 0.0042 lux/count
    float base = 0.0042f;
    // Scale by IT ratio
    float itScale = 100.0f / (float)itMs;
    // Scale by gain ratio
    float gainScale = 1.0f;
    switch (gain) {
        case 0: gainScale = 1.0f;    break; // 1x
        case 1: gainScale = 0.5f;   break;  // 2x (halves resolution)
        case 2: gainScale = 8.0f;   break;  // 1/8x
        case 3: gainScale = 4.0f;   break;  // 1/4x
    }
    return base * itScale * gainScale;
}

bool VEML7700Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 5000;
    _gain       = (uint8_t)(cfg["gain"]   | 0);
    _intMs      = (uint16_t)(cfg["integration_ms"] | 100);

    if (_gain > 3) _gain = 0;

    // Map integration time to register bits [5:2]
    uint8_t itBits = 0b1000; // default 100ms
    if      (_intMs <= 25)  itBits = 0b1100;
    else if (_intMs <= 50)  itBits = 0b1000;
    else if (_intMs <= 100) itBits = 0b0000;
    else if (_intMs <= 200) itBits = 0b0001;
    else if (_intMs <= 400) itBits = 0b0010;
    else                    itBits = 0b0011; // 800ms

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calLux.load(cal, "lux");
    _calWhite.load(cal, "white");

    // ALS_CONF: bits[12:11]=gain, bits[9:6]=IT, bit[0]=SD(0=on)
    uint16_t conf = ((uint16_t)_gain << 11) | ((uint16_t)itBits << 6);
    if (!_writeReg(REG_CONF, conf)) {
        DBGLN("[VEML7700] Init failed");
        return false;
    }
    delay(_intMs + 10);

    _resolution = _lookupResolution(_gain, _intMs);
    _ready = true;
    DBGF("[VEML7700] Ready gain=%d IT=%dms res=%.5f\n",
                  _gain, _intMs, _resolution);
    return true;
}

bool VEML7700Sensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int VEML7700Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 2) return 0;

    uint16_t als, white;
    if (!_readReg(REG_ALS, als))   return 0;
    if (!_readReg(REG_WHITE, white)) return 0;

    float lux = _calLux.apply((float)als * _resolution);

    // Non-linear correction for high lux (>1000 lux) per VEML7700 app note
    if (lux > 1000.0f) {
        lux = 6.0135e-13f * powf(lux, 4.0f)
            - 9.3924e-9f  * powf(lux, 3.0f)
            + 8.1488e-5f  * powf(lux, 2.0f)
            + 1.0023f     * lux;
        lux = _calLux.apply(lux); // re-apply after nonlinear correction
    }

    out[0] = SensorReading::make(0, _id, getType(), "lux",   lux,                         "lux");
    out[1] = SensorReading::make(0, _id, getType(), "white", _calWhite.apply((float)white), "counts");
    return 2;
}
