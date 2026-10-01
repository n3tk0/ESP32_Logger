// ============================================================================
// BME688_Mini — Minimal BME680/BME688 I2C driver (no Adafruit dependency)
// Based on Bosch BME680 datasheet rev 1.7 (BST-BME680-DS001); register
// indices and integer compensation follow Bosch's BME68x_SensorAPI (bme68x.c,
// bme68x_defs.h), which is the reference to check any change against.
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

        // variant_id: 0 = BME680 (gas "low"), 1 = BME688 (gas "high"). The
        // two put the gas result in different registers, use a different
        // run_gas bit and a different resistance formula.
        _gasHigh = (_read8(0xF0) == 0x01);

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

        // Enable gas measurement, select heater set-point 0.
        // run_gas is ctrl_gas_1 bits 5:4 — 01 on the BME680, 10 on the BME688.
        _write8(0x71, _gasHigh ? 0x20 : 0x10);  // nb_conv=0

        // Set temp + pressure oversampling + forced mode
        _write8(0x74, (_osrs_t << 5) | (_osrs_p << 2) | 0x01);

        // Wait for measurement to complete
        uint32_t start = millis();
        bool ready = false;
        while ((millis() - start) < 1000) {
            uint8_t status = _read8(0x1D);
            if (status & 0x80) { ready = true; break; }  // new_data_0
            delay(10);
        }
        if (!ready) return false;

        // Field 0, 0x1D..0x2D:
        //   [0]  meas_status_0     [1]  gas_meas_index_0
        //   [2..4]  press msb/lsb/xlsb    [5..7] temp msb/lsb/xlsb
        //   [8..9]  hum msb/lsb
        //   [13..14] gas_r (BME680)   [15..16] gas_r (BME688)
        //   the low byte of each gas pair carries gas_valid (0x20),
        //   heat_stab (0x10) and gas_range (0x0F)
        uint8_t buf[17];
        if (!_readBlock(0x1D, buf, sizeof(buf))) return false;

        int32_t adc_P = ((int32_t)buf[2] << 12) | ((int32_t)buf[3] << 4) | (buf[4] >> 4);
        int32_t adc_T = ((int32_t)buf[5] << 12) | ((int32_t)buf[6] << 4) | (buf[7] >> 4);
        int32_t adc_H = ((int32_t)buf[8] << 8)  | buf[9];
        const uint8_t g = _gasHigh ? 15 : 13;
        uint16_t adc_G = ((uint16_t)buf[g] << 2) | (buf[g + 1] >> 6);
        uint8_t gas_range = buf[g + 1] & 0x0F;
        bool gas_valid = (buf[g + 1] & 0x20) != 0;

        // Compensate temperature
        temperature = _calcTemp(adc_T);
        // Compensate pressure (uses _t_fine from temp calc)
        pressure = _calcPressure(adc_P);
        // Compensate humidity (uses _t_fine)
        humidity = _calcHumidity(adc_H);
        // Compensate gas resistance
        gas_resistance = !gas_valid ? 0.0f
                       : _gasHigh   ? _calcGasResHigh(adc_G, gas_range)
                                    : _calcGasRes(adc_G, gas_range);

        // The next heater set-point is computed against this ambient.
        _ambTemp = (int8_t)(temperature < -40 ? -40 : temperature > 85 ? 85 : temperature);
        return true;
    }

private:
    TwoWire* _wire = nullptr;
    uint8_t  _addr = 0x76;
    int32_t  _t_fine = 0;
    bool     _gasHigh = false;   // BME688
    int8_t   _ambTemp = 25;      // °C, for the heater set-point

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
    uint8_t  _par_H6;
    int8_t   _par_H7;
    int8_t   _par_GH1;
    int16_t  _par_GH2;
    int8_t   _par_GH3;
    uint8_t  _res_heat_range;
    int8_t   _res_heat_val;
    int8_t   _range_sw_err;

    void _readCalibration() {
        // Bosch reads three blocks into one 42-byte array; the BME68X_IDX_*
        // constants index it. c1 starts at register 0x8A (idx 0), c2 at
        // 0xE1 (idx 23), c3 at 0x00 (idx 37).
        uint8_t c1[23];  // 0x8A..0xA0
        uint8_t c2[14];  // 0xE1..0xEE
        uint8_t c3[5];   // 0x00..0x04
        _readBlock(0x8A, c1, sizeof(c1));
        _readBlock(0xE1, c2, sizeof(c2));
        _readBlock(0x00, c3, sizeof(c3));

        // Temperature
        _par_T1 = (uint16_t)(c2[9] << 8 | c2[8]);     // 0xEA:0xE9
        _par_T2 = (int16_t)(c1[1] << 8 | c1[0]);      // 0x8B:0x8A
        _par_T3 = (int8_t)c1[2];                      // 0x8C

        // Pressure
        _par_P1  = (uint16_t)(c1[5] << 8 | c1[4]);    // 0x8F:0x8E
        _par_P2  = (int16_t)(c1[7] << 8 | c1[6]);     // 0x91:0x90
        _par_P3  = (int8_t)c1[8];                     // 0x92
        _par_P4  = (int16_t)(c1[11] << 8 | c1[10]);   // 0x95:0x94
        _par_P5  = (int16_t)(c1[13] << 8 | c1[12]);   // 0x97:0x96
        _par_P7  = (int8_t)c1[14];                    // 0x98
        _par_P6  = (int8_t)c1[15];                    // 0x99
        _par_P8  = (int16_t)(c1[19] << 8 | c1[18]);   // 0x9D:0x9C
        _par_P9  = (int16_t)(c1[21] << 8 | c1[20]);   // 0x9F:0x9E
        _par_P10 = c1[22];                            // 0xA0

        // Humidity (H1 and H2 share 0xE2: H1 in the low nibble, H2 the high)
        _par_H1  = (uint16_t)(c2[2] << 4 | (c2[1] & 0x0F));  // 0xE3:0xE2<3:0>
        _par_H2  = (uint16_t)(c2[0] << 4 | (c2[1] >> 4));    // 0xE1:0xE2<7:4>
        _par_H3  = (int8_t)c2[3];     // 0xE4
        _par_H4  = (int8_t)c2[4];     // 0xE5
        _par_H5  = (int8_t)c2[5];     // 0xE6
        _par_H6  = c2[6];             // 0xE7
        _par_H7  = (int8_t)c2[7];     // 0xE8

        // Gas
        _par_GH1 = (int8_t)c2[12];                    // 0xED
        _par_GH2 = (int16_t)(c2[11] << 8 | c2[10]);   // 0xEC:0xEB
        _par_GH3 = (int8_t)c2[13];                    // 0xEE

        _res_heat_val   = (int8_t)c3[0];              // 0x00
        _res_heat_range = (c3[2] & 0x30) >> 4;        // 0x02<5:4>
        _range_sw_err   = ((int8_t)(c3[4] & 0xF0)) / 16;  // 0x04<7:4>, signed
    }

    float _calcTemp(int32_t adc_T) {
        int64_t var1 = ((int64_t)adc_T >> 3) - ((int64_t)_par_T1 << 1);
        int64_t var2 = (var1 * (int64_t)_par_T2) >> 11;
        int64_t var3 = ((var1 >> 1) * (var1 >> 1)) >> 12;
        var3 = (var3 * ((int64_t)_par_T3 * 16)) >> 14;
        _t_fine = (int32_t)(var2 + var3);
        return (float)((_t_fine * 5 + 128) >> 8) / 100.0f;
    }

    // Bosch's left shifts of signed coefficients are written as multiplies:
    // the same value, without the undefined behaviour of shifting a negative.
    float _calcPressure(int32_t adc_P) {
        int32_t var1 = (((int32_t)_t_fine) >> 1) - 64000;
        int32_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)_par_P6) >> 2;
        var2 = var2 + ((var1 * (int32_t)_par_P5) * 2);
        var2 = (var2 >> 2) + ((int32_t)_par_P4 * 65536);
        var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * ((int32_t)_par_P3 * 32)) >> 3) +
               (((int32_t)_par_P2 * var1) >> 1);
        var1 = var1 >> 18;
        var1 = ((((32768 + var1)) * (int32_t)_par_P1) >> 15);
        if (var1 == 0) return 0;

        int32_t press = 1048576 - adc_P;
        press = (int32_t)((uint32_t)(press - (var2 >> 12)) * (uint32_t)3125);
        if (press >= (int32_t)0x40000000)
            press = ((press / var1) << 1);
        else
            press = ((press << 1) / var1);

        var1 = ((int32_t)_par_P9 * ((int32_t)(((press >> 3) * (press >> 3)) >> 13))) >> 12;
        var2 = ((int32_t)(press >> 2) * (int32_t)_par_P8) >> 13;
        int32_t var3 = ((int32_t)(press >> 8) * (int32_t)(press >> 8) *
                        (int32_t)(press >> 8) * (int32_t)_par_P10) >> 17;
        press = press + ((var1 + var2 + var3 + ((int32_t)_par_P7 * 128)) >> 4);
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

    float _calcGasResHigh(uint16_t adc_gas, uint8_t gas_range) {
        uint32_t var1 = UINT32_C(262144) >> gas_range;
        int32_t  var2 = (int32_t)adc_gas - INT32_C(512);
        var2 *= INT32_C(3);
        var2 = INT32_C(4096) + var2;
        return 1000000.0f * (float)var1 / (float)var2;
    }

    uint8_t _calcHeaterRes(int targetTempC) {
        if (targetTempC < 200) targetTempC = 200;
        if (targetTempC > 400) targetTempC = 400;

        int32_t var1 = (((int32_t)_ambTemp * _par_GH3) / 1000) * 256;
        int32_t var2 = (_par_GH1 + 784) *
                       (((((_par_GH2 + 154009) * targetTempC * 5) / 100) + 3276800) / 10);
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

    bool _readBlock(uint8_t reg, uint8_t* buf, uint8_t len) {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        if (_wire->requestFrom(_addr, len) != len) {
            memset(buf, 0, len);
            return false;
        }
        for (uint8_t i = 0; i < len; i++) buf[i] = _wire->read();
        return true;
    }
};
