#include "VEML6075Sensor.h"

bool VEML6075Sensor::_writeReg16(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val & 0xFF);
    Wire.write(val >> 8);
    return Wire.endTransmission() == 0;
}

bool VEML6075Sensor::_readReg16(uint8_t reg, uint16_t& val) {
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

bool VEML6075Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 15000;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    if (sda >= 0 && scl >= 0) Wire.begin((int8_t)sda, (int8_t)scl);
    else                       Wire.begin();

    JsonObjectConst cal = cfg["calibration"];
    _calUva.load(cal, "uva");
    _calUvb.load(cal, "uvb");
    _calUvIndex.load(cal, "uv_index");

    // Verify device ID (register 0x0C should read 0x0026 for VEML6075)
    uint16_t devId = 0;
    _readReg16(0x0C, devId);
    if ((devId & 0xFF) != 0x26) {
        Serial.printf("[VEML6075] Unexpected device ID 0x%04X (expected 0x0026)\n", devId);
        // Continue anyway — some modules don't expose ID
    }

    // CONF: UV_IT=100ms (0b010), DYNAMIC=normal (0), no HD, no trigger, no AF, power-on
    // Bits [6:4]=UV_IT, bit[3]=HD, bit[2]=UV_TRIG, bit[1]=AF, bit[0]=SD
    uint16_t conf = (0b010 << 4); // 100ms integration, power on
    if (!_writeReg16(REG_CONF, conf)) {
        Serial.println("[VEML6075] Init failed");
        return false;
    }
    delay(110); // wait one integration cycle
    _ready = true;
    Serial.println("[VEML6075] Ready");
    return true;
}

bool VEML6075Sensor::read(SensorReading& out) {
    SensorReading buf[3];
    if (readAll(buf, 3) < 1) return false;
    out = buf[2]; // UV index
    return true;
}

int VEML6075Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 3) return 0;

    uint16_t rawUva, rawUvb, comp1, comp2;
    if (!_readReg16(REG_UVA, rawUva)) return 0;
    if (!_readReg16(REG_UVB, rawUvb)) return 0;
    if (!_readReg16(REG_COMP1, comp1)) return 0;
    if (!_readReg16(REG_COMP2, comp2)) return 0;

    // Compensated UVA/UVB (remove noise from visible/IR leakage)
    float uva = (float)rawUva - UVI_UVA_VIS_COEFF * (float)comp1
                               - UVI_UVA_IR_COEFF  * (float)comp2;
    float uvb = (float)rawUvb - UVI_UVB_VIS_COEFF * (float)comp1
                               - UVI_UVB_IR_COEFF  * (float)comp2;
    if (uva < 0) uva = 0;
    if (uvb < 0) uvb = 0;

    // UV Index = average of UVA and UVB weighted responses
    float uvIndex = (uva * UVI_UVA_RESP + uvb * UVI_UVB_RESP) / 2.0f;

    out[0] = SensorReading::make(0, _id, getType(), "uva",      _calUva.apply(uva),         "counts");
    out[1] = SensorReading::make(0, _id, getType(), "uvb",      _calUvb.apply(uvb),         "counts");
    out[2] = SensorReading::make(0, _id, getType(), "uv_index", _calUvIndex.apply(uvIndex), "");
    return 3;
}
