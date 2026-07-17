// ============================================================================
// BME280_Mini — Minimal BME280/BMP280 I2C driver (no Adafruit dependency)
// Based on Bosch BME280 datasheet rev 1.9 (BST-BME280-DS002)
// Supports: temperature, humidity (BME280 only), pressure
// ============================================================================
#pragma once
#include <Wire.h>

class BME280_Mini {
public:
    // Chip IDs
    static constexpr uint8_t CHIP_ID_BME280 = 0x60;
    static constexpr uint8_t CHIP_ID_BMP280_A = 0x56;
    static constexpr uint8_t CHIP_ID_BMP280_B = 0x57;
    static constexpr uint8_t CHIP_ID_BMP280_C = 0x58;

    bool begin(uint8_t addr = 0x76, TwoWire* wire = &Wire) {
        _addr = addr;
        _wire = wire;

        // Read chip ID (register 0xD0)
        _chipId = _read8(0xD0);
        if (_chipId != CHIP_ID_BME280 &&
            _chipId != CHIP_ID_BMP280_A &&
            _chipId != CHIP_ID_BMP280_B &&
            _chipId != CHIP_ID_BMP280_C) {
            return false;
        }
        _isBME280 = (_chipId == CHIP_ID_BME280);

        // Soft reset
        _write8(0xE0, 0xB6);
        delay(10);

        // Wait for NVM copy (up to 100 ms; return false if sensor hangs)
        bool nvm_ready = false;
        for (int i = 0; i < 100; i++) {
            if ((_read8(0xF3) & 0x01) == 0) { nvm_ready = true; break; }
            delay(1);
        }
        if (!nvm_ready) return false;

        // Read calibration data
        _readCalibration();

        // Configure: normal mode, 16x oversampling for all
        _write8(0xF2, 0x05);       // ctrl_hum: osrs_h = 16x (only BME280)
        _write8(0xF5, 0x00);       // config: standby 0.5ms, filter off
        _write8(0xF4, 0xB7);       // ctrl_meas: osrs_t=16x, osrs_p=16x, normal mode
        delay(50);

        return true;
    }

    bool isBME280() const { return _isBME280; }
    uint8_t chipId() const { return _chipId; }

    float readTemperature() {
        int32_t adc_T = _read24(0xFA) >> 4;
        if (adc_T == 0x80000) return NAN;  // skipped/error

        int32_t var1 = ((((adc_T >> 3) - ((int32_t)_dig_T1 << 1))) *
                        ((int32_t)_dig_T2)) >> 11;
        int32_t var2 = (((((adc_T >> 4) - ((int32_t)_dig_T1)) *
                          ((adc_T >> 4) - ((int32_t)_dig_T1))) >> 12) *
                        ((int32_t)_dig_T3)) >> 14;
        _t_fine = var1 + var2;
        return (float)((_t_fine * 5 + 128) >> 8) / 100.0f;
    }

    float readPressure() {
        if (_t_fine == 0) readTemperature();  // guard: ensure compensation is ready
        int32_t adc_P = _read24(0xF7) >> 4;
        if (adc_P == 0x80000) return NAN;

        int64_t var1 = ((int64_t)_t_fine) - 128000;
        int64_t var2 = var1 * var1 * (int64_t)_dig_P6;
        var2 = var2 + ((var1 * (int64_t)_dig_P5) << 17);
        var2 = var2 + (((int64_t)_dig_P4) << 35);
        var1 = ((var1 * var1 * (int64_t)_dig_P3) >> 8) +
               ((var1 * (int64_t)_dig_P2) << 12);
        var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)_dig_P1) >> 33;
        if (var1 == 0) return 0;

        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - var2) * 3125) / var1;
        var1 = (((int64_t)_dig_P9) * (p >> 13) * (p >> 13)) >> 25;
        var2 = (((int64_t)_dig_P8) * p) >> 19;
        p = ((p + var1 + var2) >> 8) + (((int64_t)_dig_P7) << 4);
        return (float)p / 256.0f;  // Pa
    }

    float readHumidity() {
        if (!_isBME280) return NAN;
        if (_t_fine == 0) readTemperature();  // guard: ensure compensation is ready
        int32_t adc_H = _read16(0xFD);
        if (adc_H == 0x8000) return NAN;      // skipped/disabled


        int32_t var1, var2, var3, var4, var5;
        var1 = _t_fine - ((int32_t)76800);
        var2 = (int32_t)(adc_H * 16384);
        var3 = (int32_t)(((int32_t)_dig_H4) * 1048576);
        var4 = ((int32_t)_dig_H5) * var1;
        var5 = (((var2 - var3) - var4) + (int32_t)16384) / 32768;
        var2 = (var1 * ((int32_t)_dig_H6)) / 1024;
        var3 = (var1 * ((int32_t)_dig_H3)) / 2048;
        var4 = ((var2 * (var3 + (int32_t)32768)) / 1024) + (int32_t)2097152;
        var2 = ((var4 * ((int32_t)_dig_H2)) + 8192) / 16384;
        var3 = var5 * var2;
        var4 = ((var3 / 32768) * (var3 / 32768)) / 128;
        var5 = var3 - ((var4 * ((int32_t)_dig_H1)) / 16);
        var5 = (var5 < 0 ? 0 : var5);
        var5 = (var5 > 419430400 ? 419430400 : var5);
        uint32_t H = (uint32_t)(var5 / 4096);
        return (float)H / 1024.0f;
    }

private:
    TwoWire* _wire = nullptr;
    uint8_t  _addr = 0x76;
    uint8_t  _chipId = 0;
    bool     _isBME280 = false;
    int32_t  _t_fine = 0;

    // Temperature calibration
    uint16_t _dig_T1 = 0;
    int16_t  _dig_T2 = 0, _dig_T3 = 0;
    // Pressure calibration
    uint16_t _dig_P1 = 0;
    int16_t  _dig_P2 = 0, _dig_P3 = 0, _dig_P4 = 0, _dig_P5 = 0;
    int16_t  _dig_P6 = 0, _dig_P7 = 0, _dig_P8 = 0, _dig_P9 = 0;
    // Humidity calibration (BME280 only)
    uint8_t  _dig_H1 = 0, _dig_H3 = 0;
    int16_t  _dig_H2 = 0, _dig_H4 = 0, _dig_H5 = 0;
    int8_t   _dig_H6 = 0;

    void _readCalibration() {
        // Temperature + pressure: registers 0x88..0x9F (26 bytes)
        uint8_t buf[26] = {0};
        _readBlock(0x88, buf, 26);

        _dig_T1 = (uint16_t)(buf[1] << 8 | buf[0]);
        _dig_T2 = (int16_t)(buf[3] << 8 | buf[2]);
        _dig_T3 = (int16_t)(buf[5] << 8 | buf[4]);

        _dig_P1 = (uint16_t)(buf[7]  << 8 | buf[6]);
        _dig_P2 = (int16_t)(buf[9]  << 8 | buf[8]);
        _dig_P3 = (int16_t)(buf[11] << 8 | buf[10]);
        _dig_P4 = (int16_t)(buf[13] << 8 | buf[12]);
        _dig_P5 = (int16_t)(buf[15] << 8 | buf[14]);
        _dig_P6 = (int16_t)(buf[17] << 8 | buf[16]);
        _dig_P7 = (int16_t)(buf[19] << 8 | buf[18]);
        _dig_P8 = (int16_t)(buf[21] << 8 | buf[20]);
        _dig_P9 = (int16_t)(buf[23] << 8 | buf[22]);

        if (!_isBME280) return;

        // Humidity: 0xA1 (1 byte) + 0xE1..0xE7 (7 bytes)
        _dig_H1 = _read8(0xA1);
        uint8_t hbuf[7] = {0};
        _readBlock(0xE1, hbuf, 7);
        _dig_H2 = (int16_t)(hbuf[1] << 8 | hbuf[0]);
        _dig_H3 = hbuf[2];
        // dig_H4: 12-bit signed, [11:4] in 0xE4 (hbuf[3]), [3:0] in lower nibble of 0xE5 (hbuf[4])
        _dig_H4 = (int16_t)((uint16_t)hbuf[3] << 4 | (hbuf[4] & 0x0F));
        _dig_H4 = (int16_t)(_dig_H4 << 4) >> 4;
        // dig_H5: 12-bit signed, [3:0] in upper nibble of 0xE5 (hbuf[4]), [11:4] in 0xE6 (hbuf[5])
        _dig_H5 = (int16_t)((uint16_t)hbuf[5] << 4 | (hbuf[4] >> 4));
        _dig_H5 = (int16_t)(_dig_H5 << 4) >> 4;
        _dig_H6 = (int8_t)hbuf[6];
    }

    // --- Low-level I2C helpers ---
    void _write8(uint8_t reg, uint8_t val) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }

    uint8_t _read8(uint8_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, (uint8_t)1);
        if (_wire->available() < 1) return 0xFF;  // short read → invalid chip id / status
        return _wire->read();
    }

    uint16_t _read16(uint8_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, (uint8_t)2);
        if (_wire->available() < 2) return 0x8000;  // short read → humidity skip sentinel
        uint16_t val = (uint16_t)_wire->read() << 8;
        val |= _wire->read();
        return val;
    }

    int32_t _read24(uint8_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, (uint8_t)3);
        if (_wire->available() < 3) return 0x800000;  // short read → (>>4)=0x80000 skip sentinel
        int32_t val = (int32_t)_wire->read() << 16;
        val |= (int32_t)_wire->read() << 8;
        val |= _wire->read();
        return val;
    }

    void _readBlock(uint8_t reg, uint8_t* buf, uint8_t len) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, len);
        for (uint8_t i = 0; i < len && _wire->available(); i++) {
            buf[i] = _wire->read();
        }
    }
};
