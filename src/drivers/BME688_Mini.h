// ============================================================================
// BME688_Mini — Minimal BME680/BME688 I2C driver (no Adafruit dependency)
// Based on Bosch BME680 datasheet rev 1.7 (BST-BME680-DS001)
// Supports: temperature, humidity, pressure, gas resistance
// ============================================================================
#pragma once
#include <Wire.h>

class BME688_Mini {
public:
    // Oversampling constants (register field values)
    static constexpr uint8_t OS_NONE = 0;
    static constexpr uint8_t OS_1X   = 1;
    static constexpr uint8_t OS_2X   = 2;
    static constexpr uint8_t OS_4X   = 3;
    static constexpr uint8_t OS_8X   = 4;
    static constexpr uint8_t OS_16X  = 5;

    // IIR filter coefficients
    static constexpr uint8_t FILTER_OFF = 0;
    static constexpr uint8_t FILTER_1   = 1;
    static constexpr uint8_t FILTER_3   = 2;
    static constexpr uint8_t FILTER_7   = 3;

    float temperature   = 0;
    float humidity      = 0;
    float pressure      = 0;
    float gas_resistance = 0;

    bool begin(uint8_t addr = 0x76, TwoWire* wire = &Wire) {
        _addr = addr;
        _wire = wire;

        uint8_t id = _read8(0xD0);
        if (id != 0x61) return false;  // BME680/688 chip ID

        // Soft reset
        _write8(0xE0, 0xB6);
        delay(10);

        _readCalibration();

        // Defaults: 8x temp, 2x hum, 4x press, IIR filter 3
        setTemperatureOversampling(OS_8X);
        setHumidityOversampling(OS_2X);
        setPressureOversampling(OS_4X);
        setIIRFilterSize(FILTER_3);
        setGasHeater(320, 150);

        return true;
    }

    void setTemperatureOversampling(uint8_t os) { _osrs_t = os & 0x07; }
    void setHumidityOversampling(uint8_t os)    { _osrs_h = os & 0x07; }
    void setPressureOversampling(uint8_t os)    { _osrs_p = os & 0x07; }
    void setIIRFilterSize(uint8_t f)            { _filter = f & 0x07; }

    void setGasHeater(int targetTempC, int durationMs) {
        _heaterTemp = targetTempC;
        _heaterDur  = durationMs;
    }

    bool performReading() {
        // Set humidity oversampling (must be written before ctrl_meas)
        _write8(0x72, _osrs_h);

        // Set IIR filter
        uint8_t cfgReg = _read8(0x75);
        cfgReg = (cfgReg & 0xE3) | (_filter << 2);
        _write8(0x75, cfgReg);

        // Set gas heater
        _write8(0x5A, _calcHeaterRes(_heaterTemp));   // res_heat_0
        _write8(0x64, _calcHeaterDur(_heaterDur));     // gas_wait_0

        // Enable gas measurement, select heater set-point 0
        _write8(0x71, 0x10);  // run_gas=1, nb_conv=0

        // Set temp + pressure oversampling + forced mode
        _write8(0x74, (_osrs_t << 5) | (_osrs_p << 2) | 0x01);

        // Wait for measurement to complete
        uint32_t start = millis();
        while ((millis() - start) < 1000) {
            uint8_t status = _read8(0x1D);
            if (status & 0x80) break;  // new_data_0
            delay(10);
        }

        // Read raw data
        uint8_t buf[15];
        _readBlock(0x1D, buf, 15);

        // buf[0]=meas_status, buf[1..2]=press_msb/lsb, buf[3]=press_xlsb
        // buf[4..5]=temp_msb/lsb, buf[6]=temp_xlsb
        // buf[7..8]=hum_msb/lsb
        // buf[13]=gas_r_msb, buf[14]=gas_r_lsb (with gas_valid/heat_stab bits)

        int32_t adc_T = ((int32_t)buf[4] << 12) | ((int32_t)buf[5] << 4) | (buf[6] >> 4);
        int32_t adc_P = ((int32_t)buf[1] << 12) | ((int32_t)buf[2] << 4) | (buf[3] >> 4);
        int32_t adc_H = ((int32_t)buf[7] << 8)  | buf[8];
        uint16_t adc_G = ((uint16_t)buf[13] << 2) | (buf[14] >> 6);
        uint8_t gas_range = buf[14] & 0x0F;
        bool gas_valid = (buf[14] & 0x20) != 0;

        // Compensate temperature
        temperature = _calcTemp(adc_T);
        // Compensate pressure (uses _t_fine from temp calc)
        pressure = _calcPressure(adc_P);
        // Compensate humidity (uses _t_fine)
        humidity = _calcHumidity(adc_H);
        // Compensate gas resistance
        gas_resistance = gas_valid ? _calcGasRes(adc_G, gas_range) : 0.0f;

        return true;
    }

private:
    TwoWire* _wire = nullptr;
    uint8_t  _addr = 0x76;
    int32_t  _t_fine = 0;

    uint8_t _osrs_t = OS_8X;
    uint8_t _osrs_h = OS_2X;
    uint8_t _osrs_p = OS_4X;
    uint8_t _filter = FILTER_3;
    int     _heaterTemp = 320;
    int     _heaterDur  = 150;

    // Calibration coefficients
    uint16_t _par_T1;
    int16_t  _par_T2;
    int8_t   _par_T3;
    uint16_t _par_P1;
    int16_t  _par_P2, _par_P3, _par_P4, _par_P5;
    int16_t  _par_P6, _par_P7;
    int16_t  _par_P8, _par_P9;
    uint8_t  _par_P10;
    uint16_t _par_H1, _par_H2;
    int8_t   _par_H3, _par_H4, _par_H5;
    uint8_t  _par_H6, _par_H7;
    int8_t   _par_GH1;
    int16_t  _par_GH2;
    int8_t   _par_GH3;
    uint8_t  _res_heat_range;
    int8_t   _res_heat_val;
    int8_t   _range_sw_err;

    void _readCalibration() {
        // Coefficients from registers 0x8A..0xA1 and 0xE1..0xF0
        uint8_t coeff1[25]; // 0x8A..0xA2 (25 bytes)
        _readBlock(0x8A, coeff1, 25);

        uint8_t coeff2[16]; // 0xE1..0xF0 (16 bytes)
        _readBlock(0xE1, coeff2, 16);

        // Temperature
        _par_T1 = (uint16_t)(coeff2[9] << 8 | coeff2[8]);    // 0xEA:0xE9
        _par_T2 = (int16_t)(coeff1[2]  << 8 | coeff1[1]);    // 0x8C:0x8B
        _par_T3 = (int8_t)coeff1[3];                          // 0x8D

        // Pressure
        _par_P1  = (uint16_t)(coeff1[6]  << 8 | coeff1[5]);  // 0x90:0x8F
        _par_P2  = (int16_t)(coeff1[8]  << 8 | coeff1[7]);   // 0x92:0x91
        _par_P3  = (int8_t)coeff1[9];                         // 0x93
        _par_P4  = (int16_t)(coeff1[12] << 8 | coeff1[11]);  // 0x96:0x95
        _par_P5  = (int16_t)(coeff1[14] << 8 | coeff1[13]);  // 0x98:0x97
        _par_P6  = (int8_t)coeff1[16];                        // 0x9A? Actually _par_P6 is at 0x99
        _par_P7  = (int8_t)coeff1[15];                        // 0x98? Let me re-derive
        // Re-do using Bosch API register map:
        _par_P6  = (int8_t)coeff1[16];     // 0x9A  (Note: coeff1[0] = reg 0x8A)
        _par_P7  = (int8_t)coeff1[15];     // 0x99
        _par_P8  = (int16_t)(coeff1[20] << 8 | coeff1[19]);  // 0x9E:0x9D
        _par_P9  = (int16_t)(coeff1[22] << 8 | coeff1[21]);  // 0xA0:0x9F
        _par_P10 = coeff1[23];                                 // 0xA1

        // Humidity
        _par_H1  = (uint16_t)(coeff2[2] << 4 | (coeff2[1] & 0x0F));  // 0xE3:0xE2<3:0>
        _par_H2  = (uint16_t)(coeff2[0] << 4 | (coeff2[1] >> 4));    // 0xE1:0xE2<7:4>
        _par_H3  = (int8_t)coeff2[3];     // 0xE4
        _par_H4  = (int8_t)coeff2[4];     // 0xE5
        _par_H5  = (int8_t)coeff2[5];     // 0xE6
        _par_H6  = coeff2[6];              // 0xE7
        _par_H7  = (int8_t)coeff2[7];     // 0xE8

        // Gas
        _par_GH1 = (int8_t)coeff2[12];    // 0xED
        _par_GH2 = (int16_t)(coeff2[11] << 8 | coeff2[10]); // 0xEC:0xEB
        _par_GH3 = (int8_t)coeff2[13];    // 0xEE

        _res_heat_range = (_read8(0x02) >> 4) & 0x03;
        _res_heat_val   = (int8_t)_read8(0x00);
        _range_sw_err   = ((int8_t)(_read8(0x04))) >> 4;
    }

    float _calcTemp(int32_t adc_T) {
        int64_t var1 = ((int64_t)adc_T >> 3) - ((int64_t)_par_T1 << 1);
        int64_t var2 = (var1 * (int64_t)_par_T2) >> 11;
        int64_t var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
        var3 = (var3 * ((int64_t)_par_T3 << 4)) >> 14;
        _t_fine = (int32_t)(var2 + var3);
        return (float)((_t_fine * 5 + 128) >> 8) / 100.0f;
    }

    float _calcPressure(int32_t adc_P) {
        int32_t var1 = (((int32_t)_t_fine) >> 1) - 64000;
        int32_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)_par_P6) >> 2;
        var2 = var2 + ((var1 * (int32_t)_par_P5) << 1);
        var2 = (var2 >> 2) + ((int32_t)_par_P4 << 16);
        var1 = (((((int32_t)_par_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) +
                 (((int32_t)_par_P2 * var1) >> 1)) >> 18);
        var1 = ((((32768 + var1)) * (int32_t)_par_P1) >> 15);
        if (var1 == 0) return 0;

        int32_t press = (int32_t)(((uint32_t)(((int32_t)1048576) - adc_P) - (var2 >> 12))) * 3125;
        if (press >= (int32_t)0x40000000)
            press = ((press / (uint32_t)var1) << 1);
        else
            press = ((press << 1) / (uint32_t)var1);

        var1 = ((int32_t)_par_P9 * ((int32_t)(((press >> 3) * (press >> 3)) >> 13))) >> 12;
        var2 = ((int32_t)(press >> 2) * (int32_t)_par_P8) >> 13;
        int32_t var3 = ((int32_t)(press >> 8) * (int32_t)(press >> 8) *
                        (int32_t)(press >> 8) * (int32_t)_par_P10) >> 17;
        press = press + ((var1 + var2 + var3 + ((int32_t)_par_P7 << 7)) >> 4);
        return (float)press;  // Pa
    }

    float _calcHumidity(int32_t adc_H) {
        int32_t temp_scaled = (int32_t)((_t_fine * 5 + 128) >> 8);
        int32_t var1 = (int32_t)(adc_H - ((int32_t)((int32_t)_par_H1 * 16))) -
                       (((temp_scaled * (int32_t)_par_H3) / ((int32_t)100)) >> 1);
        int32_t var2 = ((int32_t)_par_H2 *
                        (((temp_scaled * (int32_t)_par_H4) / ((int32_t)100)) +
                         (((temp_scaled * ((temp_scaled * (int32_t)_par_H5) /
                          ((int32_t)100))) >> 6) / ((int32_t)100)) + (int32_t)(1 << 14))) >> 10;
        int32_t var3 = var1 * var2;
        int32_t var4 = (int32_t)_par_H6 << 7;
        var4 = ((var4) + ((temp_scaled * (int32_t)_par_H7) / ((int32_t)100))) >> 4;
        int32_t var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
        int32_t var6 = (var4 * var5) >> 1;
        float hum = (float)(((var3 + var6) >> 10) * ((int32_t)1000)) / (float)((int32_t)4096 * 1000);
        if (hum > 100.0f) hum = 100.0f;
        if (hum < 0.0f)   hum = 0.0f;
        return hum;
    }

    float _calcGasRes(uint16_t adc_gas, uint8_t gas_range) {
        // Lookup tables from Bosch driver
        static const uint32_t lookupK1[] = {
            UINT32_C(2147483647), UINT32_C(2147483647), UINT32_C(2147483647), UINT32_C(2147483647),
            UINT32_C(2147483647), UINT32_C(2126008810), UINT32_C(2147483647), UINT32_C(2130303777),
            UINT32_C(2147483647), UINT32_C(2147483647), UINT32_C(2143188679), UINT32_C(2136746228),
            UINT32_C(2147483647), UINT32_C(2126008810), UINT32_C(2147483647), UINT32_C(2147483647)
        };
        static const uint32_t lookupK2[] = {
            UINT32_C(4096000000), UINT32_C(2048000000), UINT32_C(1024000000), UINT32_C(512000000),
            UINT32_C(255744255),  UINT32_C(127110228),  UINT32_C(64000000),   UINT32_C(32258064),
            UINT32_C(16016016),   UINT32_C(8000000),    UINT32_C(4000000),    UINT32_C(2000000),
            UINT32_C(1000000),    UINT32_C(500000),     UINT32_C(250000),     UINT32_C(125000)
        };
        int64_t var1 = (int64_t)((1340 + (5 * (int64_t)_range_sw_err)) *
                       ((int64_t)lookupK1[gas_range])) >> 16;
        int64_t var2 = (((int64_t)((int64_t)adc_gas << 15) - (int64_t)(16777216)) + var1);
        int64_t var3 = (((int64_t)lookupK2[gas_range] * (int64_t)var1) >> 9);
        return (float)((var3 + ((int64_t)var2 >> 1)) / (int64_t)var2);
    }

    uint8_t _calcHeaterRes(int targetTempC) {
        // Approximate heater resistance calculation
        if (targetTempC < 200) targetTempC = 200;
        if (targetTempC > 400) targetTempC = 400;

        int32_t var1 = (((int32_t)25 - (int32_t)_par_GH3) * 1000) / 20;
        int32_t var2 = (((int32_t)_par_GH1 * 1000) + 784) *
                       (((((int32_t)_par_GH2 + 154009) * targetTempC * 5) / 100) + 3276800) / 10;
        int32_t var3 = var1 + (var2 / 2);
        int32_t var4 = (var3 / (_res_heat_range + 4));
        int32_t var5 = (131 * _res_heat_val) + 65536;
        int32_t heatr_res_x100 = (int32_t)(((var4 / var5) - 250) * 34);
        return (uint8_t)((heatr_res_x100 + 50) / 100);
    }

    uint8_t _calcHeaterDur(int durMs) {
        uint8_t factor = 0;
        uint8_t durval;
        if (durMs >= 0xFC0) {
            durval = 0xFF;
        } else {
            while (durMs > 0x3F) {
                durMs /= 4;
                factor++;
            }
            durval = (uint8_t)(durMs + (factor * 64));
        }
        return durval;
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
        return _wire->read();
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
