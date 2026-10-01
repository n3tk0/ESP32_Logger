// Host tests for src/drivers/BME688_Mini.h — the BME680/BME688 driver.
//
// The register image below is a plausible BME680 calibration plus one
// measurement. Every expected value was produced by Bosch's own
// BME68x_SensorAPI (bme68x.c, integer build, BME68X_DO_NOT_USE_FPU) reading
// the same image, so these checks pin the driver to the reference rather
// than to itself. The driver used to read the first calibration block one
// byte late, the result field from the gas-index byte, read par_h7 as
// unsigned and dropped the `<< 5` on par_p3: on a real BME680 that gave
// -2.4 °C, 100 % and 710 hPa.
#include "check.h"
#include <Wire.h>
#include "src/drivers/BME688_Mini.h"

#define W16(r, lo, v) do { int _v = (int)(v); (r)[lo] = _v & 0xFF; (r)[(lo) + 1] = (_v >> 8) & 0xFF; } while (0)

static void fillImage(uint8_t* r, bool gasHigh) {
    memset(r, 0, 256);
    r[0xD0] = 0x61;                 // chip id
    r[0xF0] = gasHigh ? 1 : 0;      // variant: BME688 / BME680
    // Temperature: T1 at 0xE9, T2 at 0x8A, T3 at 0x8C
    W16(r, 0xE9, 26147); W16(r, 0x8A, 26383); r[0x8C] = 3;
    // Pressure: P1..P10 from 0x8E
    W16(r, 0x8E, 36175); W16(r, 0x90, -10461); r[0x92] = 88;
    W16(r, 0x94, 6574);  W16(r, 0x96, -108);   r[0x98] = 38; r[0x99] = 30;
    W16(r, 0x9C, -1335); W16(r, 0x9E, -2765);  r[0xA0] = 30;
    // Humidity: H1 = 759, H2 = 1031 sharing 0xE2
    r[0xE1] = 1031 >> 4; r[0xE2] = ((1031 & 0x0F) << 4) | (759 & 0x0F); r[0xE3] = 759 >> 4;
    r[0xE4] = 0; r[0xE5] = 45; r[0xE6] = 20; r[0xE7] = 120; r[0xE8] = (uint8_t)-100;
    // Gas heater
    W16(r, 0xEB, -12519); r[0xED] = (uint8_t)-30; r[0xEE] = 18;
    r[0x00] = 45;      // res_heat_val
    r[0x02] = 0x10;    // res_heat_range = 1
    r[0x04] = 0x10;    // range_switching_error = 1

    // Field 0: new_data, adc_P = 400000, adc_T = 513000, adc_H = 23000,
    // gas adc 500 in range 5, valid and stable.
    const int32_t P = 400000, T = 513000; const uint16_t H = 23000, G = 500;
    r[0x1D] = 0x80;
    r[0x1F] = P >> 12; r[0x20] = (P >> 4) & 0xFF; r[0x21] = (P & 0x0F) << 4;
    r[0x22] = T >> 12; r[0x23] = (T >> 4) & 0xFF; r[0x24] = (T & 0x0F) << 4;
    r[0x25] = H >> 8;  r[0x26] = H & 0xFF;
    const int g = gasHigh ? 0x2C : 0x2A;
    r[g] = G >> 2; r[g + 1] = ((G & 3) << 6) | 0x20 | 0x10 | 5;
}

static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static void test_bme680_matches_bosch() {
    TwoWire w; w.addr = 0x77;
    fillImage(w.regs, false);
    BME688_Mini bme;
    CHECK(bme.begin(0x77, &w));
    CHECK(bme.performReading());
    CHECK(near(bme.temperature, 29.77f, 0.005f));     // Bosch: 2977 (x100 °C)
    CHECK(near(bme.pressure, 94673.0f, 0.5f));        // Bosch: 94673 Pa
    CHECK(near(bme.humidity, 59.878f, 0.001f));       // Bosch: 59878 (x1000 %)
    CHECK(near(bme.gas_resistance, 250520.0f, 1.0f)); // Bosch: 250520 Ω
    CHECK(w.regs[0x5A] == 114);                       // res_heat_0 at 25 °C ambient
    CHECK(w.regs[0x64] == 0x65);                      // gas_wait_0 for 150 ms
    CHECK(w.regs[0x71] == 0x10);                      // run_gas, BME680 encoding
}

static void test_bme688_gas_registers_and_formula() {
    TwoWire w;
    fillImage(w.regs, true);
    BME688_Mini bme;
    CHECK(bme.begin(0x76, &w));
    CHECK(bme.performReading());
    CHECK(near(bme.temperature, 29.77f, 0.005f));
    CHECK(w.regs[0x71] == 0x20);                      // run_gas, BME688 encoding
    // Bosch's integer build rounds to 100 Ω: 2017700.
    CHECK(near(bme.gas_resistance, 2017700.0f, 100.0f));
}

static void test_not_found_and_no_data() {
    TwoWire w; w.addr = 0x77;
    fillImage(w.regs, false);
    BME688_Mini bme;
    CHECK(!bme.begin(0x76, &w));                      // nobody at 0x76

    TwoWire w2;
    fillImage(w2.regs, false);
    w2.regs[0x1D] = 0;                                // never reports new data
    BME688_Mini b2;
    CHECK(b2.begin(0x76, &w2));
    CHECK(!b2.performReading());
}

int main() {
    RUN(test_bme680_matches_bosch);
    RUN(test_bme688_gas_registers_and_formula);
    RUN(test_not_found_and_no_data);
    return SUMMARY();
}
