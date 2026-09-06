#include "sensors.h"

#include <math.h>
#include <Wire.h>

#include "node_config.h"
#include "NodePins.h"   // the silkscreen label for a GPIO, for the log lines

#if defined(NODE_SENSOR_BMX280)
#  include "src/drivers/BME280_Mini.h"
#endif
#if defined(NODE_SENSOR_BME688)
#  include "src/drivers/BME688_Mini.h"
#endif
#if defined(NODE_SENSOR_DS18B20)
// DS18B20_Mini.h guards each bit of its 1-Wire timing with the FreeRTOS
// portDISABLE_INTERRUPTS / portENABLE_INTERRUPTS pair, which the ESP8266 core
// does not define. They map cleanly onto the Arduino pair it does provide —
// the driver only needs "nothing preempts this timed sequence".
//
// Shimmed here rather than changed in the driver: that file is the
// collector's, it is on a shipped code path, and portENTER/portEXIT have
// per-core semantics on the ESP32 that its author chose deliberately.
//
// Safe on this part despite WiFi: the critical sections are one bit each,
// roughly 70 µs, far short of the window where the ESP8266 starts dropping
// beacons. A whole-frame lock would not be.
#  ifndef portDISABLE_INTERRUPTS
#    define portDISABLE_INTERRUPTS() noInterrupts()
#  endif
#  ifndef portENABLE_INTERRUPTS
#    define portENABLE_INTERRUPTS()  interrupts()
#  endif
#  include "src/drivers/DS18B20_Mini.h"
#endif

#if defined(NODE_SENSOR_SDS011)
#  include <SoftwareSerial.h>
#endif

#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688) || \
    defined(NODE_SENSOR_BH1750)
#  define NODE_HAS_I2C 1
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
#if defined(NODE_SENSOR_BMX280)
static BME280_Mini s_bmx;
static bool        s_bmxOk = false;
#endif
#if defined(NODE_SENSOR_BME688)
static BME688_Mini s_bme688;
static bool        s_bme688Ok = false;
#endif
#if defined(NODE_SENSOR_DS18B20)
static DS18B20_Mini s_ds;
static int          s_dsCount = 0;
#endif

#if defined(NODE_SENSOR_BH1750)
static bool s_bhOk = false;
// Continuous high-resolution mode; 1.2 counts per lux, per the datasheet and
// matching the collector's BH1750 plugin so the two agree.
static constexpr uint8_t  BH1750_CMD_POWER_ON  = 0x01;
static constexpr uint8_t  BH1750_CMD_CONT_HRES = 0x10;
static constexpr float    BH1750_DIVIDER       = 1.2f;
static uint8_t            s_bhAddr             = BH1750_ADDR;
#endif

#if defined(NODE_SENSOR_SDS011)
static SoftwareSerial s_sds;
static bool  s_sdsOk    = false;
static float s_sdsPm25  = NAN;
static float s_sdsPm10  = NAN;
#endif

#if defined(NODE_SENSOR_PULSE)
// Touched by the ISR, so volatile and read under a brief interrupt lock.
static volatile uint32_t s_pulses      = 0;
static volatile uint32_t s_lastPulseUs = 0;
static uint32_t          s_lastDrainMs = 0;
static float             s_pulseTotal  = 0.0f;
static bool              s_pulseOk     = false;

// Must live in IRAM: the ESP8266 services interrupts while the flash cache is
// busy, and an ISR in flash faults the moment it is called during a read.
static void IRAM_ATTR pulseIsr() {
    const uint32_t now = micros();
#if PULSE_DEBOUNCE_US > 0
    // Contact bounce on a reed switch arrives as a burst; anything closer
    // than the debounce window is the same tip.
    if ((uint32_t)(now - s_lastPulseUs) < PULSE_DEBOUNCE_US) return;
#endif
    s_lastPulseUs = now;
    s_pulses++;
}
#endif

static char s_describe[64] = "none";

// ---------------------------------------------------------------------------
// Sea-level pressure
// ---------------------------------------------------------------------------
// Station pressure falls about 12 Pa per metre near sea level, so a node 300 m
// up reads ~35 hPa below what a forecast quotes. Without the conversion,
// comparing the node's reading against a forecast's figure compares two
// different quantities.
static float toSeaLevel(float stationHpa, float tempC, float altitudeM) {
    if (altitudeM == 0.0f || !isfinite(stationHpa) || !isfinite(tempC)) return NAN;
    return stationHpa * powf(1.0f - (0.0065f * altitudeM)
                                    / (tempC + 0.0065f * altitudeM + 273.15f),
                             -5.257f);
}

static void add(NodeReading* out, int& n, int maxOut,
                const char* metric, float value, const char* unit) {
    if (n >= maxOut)     return;
    if (!isfinite(value)) return;   // omit rather than send a NaN JSON cannot carry
    out[n].metric = metric;
    out[n].value  = value;
    out[n].unit   = unit;
    n++;
}

#ifdef NODE_HAS_I2C
/// "GPIO12 (D6)" — both ways of writing the same pin, because a wiring mistake
/// is the likeliest reason anyone is reading these lines, and the two numbering
/// schemes are exactly what people mix up.
static void describePin(char* out, size_t n, uint8_t gpio) {
    const char* d = NodePins::dLabelFor(gpio);
    if (*d) snprintf(out, n, "GPIO%u (%s)", (unsigned)gpio, d);
    else    snprintf(out, n, "GPIO%u", (unsigned)gpio);
}
#endif

// ---------------------------------------------------------------------------
// begin
// ---------------------------------------------------------------------------
int sensorsBegin(const NodeSettings& s) {
    int ok = 0;
    s_describe[0] = '\0';

#ifdef NODE_HAS_I2C
    Wire.begin(s.i2cSda, s.i2cScl);
    // Printed on every probe, success or not. "No sensor found" and "no sensor
    // found ON THESE TWO PINS" are the same sentence to the firmware and very
    // different ones to whoever is holding the board: the commonest cause is a
    // silkscreen D-number entered as a GPIO, and this line is where that shows.
    {
        char sdaTxt[20], sclTxt[20];
        describePin(sdaTxt, sizeof(sdaTxt), s.i2cSda);
        describePin(sclTxt, sizeof(sclTxt), s.i2cScl);
        Serial.printf("[sensor] I2C on SDA=%s SCL=%s\n", sdaTxt, sclTxt);
    }
#endif

#if defined(NODE_SENSOR_BMX280)
    if (!s_bmxOk) {
        // Breakouts strap SDO either way; try the configured address first,
        // then the other, so a board that shipped as 0x77 works untouched.
        const uint8_t cand[2] = { BMX280_ADDR,
                                  (uint8_t)(BMX280_ADDR == 0x76 ? 0x77 : 0x76) };
        for (uint8_t i = 0; i < 2 && !s_bmxOk; i++) {
            if (s_bmx.begin(cand[i], &Wire)) {
                s_bmxOk = true;
                Serial.printf("[sensor] %s at 0x%02X on SDA=%u SCL=%u\n",
                              s_bmx.isBME280() ? "BME280" : "BMP280",
                              cand[i], s.i2cSda, s.i2cScl);
            }
        }
        if (!s_bmxOk) {
            char sdaTxt[20], sclTxt[20];
            describePin(sdaTxt, sizeof(sdaTxt), s.i2cSda);
            describePin(sclTxt, sizeof(sclTxt), s.i2cScl);
            Serial.printf("[sensor] no BME280/BMP280 at 0x%02X or 0x%02X "
                          "(SDA=%s SCL=%s)\n",
                          cand[0], cand[1], sdaTxt, sclTxt);
        }
    }
    if (s_bmxOk) {
        ok++;
        strncat(s_describe, s_bmx.isBME280() ? "BME280" : "BMP280",
                sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

#if defined(NODE_SENSOR_BME688)
    if (!s_bme688Ok) {
        const uint8_t cand[2] = { BMX280_ADDR,
                                  (uint8_t)(BMX280_ADDR == 0x76 ? 0x77 : 0x76) };
        for (uint8_t i = 0; i < 2 && !s_bme688Ok; i++) {
            if (s_bme688.begin(cand[i], &Wire)) {
                s_bme688Ok = true;
                Serial.printf("[sensor] BME688 at 0x%02X on SDA=%u SCL=%u\n",
                              cand[i], s.i2cSda, s.i2cScl);
            }
        }
        if (!s_bme688Ok) {
            char sdaTxt[20], sclTxt[20];
            describePin(sdaTxt, sizeof(sdaTxt), s.i2cSda);
            describePin(sclTxt, sizeof(sclTxt), s.i2cScl);
            Serial.printf("[sensor] no BME680/BME688 at 0x%02X or 0x%02X "
                          "(SDA=%s SCL=%s)\n",
                          cand[0], cand[1], sdaTxt, sclTxt);
        }
    }
    if (s_bme688Ok) {
        ok++;
        strncat(s_describe, "BME688", sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

#if defined(NODE_SENSOR_DS18B20)
    if (s_dsCount == 0) {
        if (s_ds.begin(s.oneWirePin)) {
            s_dsCount = s_ds.deviceCount();
            Serial.printf("[sensor] DS18B20 x%d on GPIO%u\n",
                          s_dsCount, s.oneWirePin);
        } else {
            Serial.printf("[sensor] no DS18B20 on GPIO%u "
                          "(4.7k pull-up to 3V3 fitted?)\n", s.oneWirePin);
        }
    }
    if (s_dsCount > 0) {
        ok++;
        char buf[24];
        snprintf(buf, sizeof(buf), "%sDS18B20(%d)",
                 s_describe[0] ? ", " : "", s_dsCount);
        strncat(s_describe, buf, sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

#if defined(NODE_SENSOR_BH1750)
    if (!s_bhOk) {
        // ADDR low is 0x23, ADDR high 0x5C. Probe the configured one first,
        // then the other, for the same reason the BMx280 does.
        const uint8_t cand[2] = { BH1750_ADDR,
                                  (uint8_t)(BH1750_ADDR == 0x23 ? 0x5C : 0x23) };
        for (uint8_t i = 0; i < 2 && !s_bhOk; i++) {
            Wire.beginTransmission(cand[i]);
            Wire.write(BH1750_CMD_POWER_ON);
            if (Wire.endTransmission() == 0) {
                Wire.beginTransmission(cand[i]);
                Wire.write(BH1750_CMD_CONT_HRES);
                if (Wire.endTransmission() == 0) {
                    s_bhOk   = true;
                    s_bhAddr = cand[i];
                    // First continuous conversion takes up to 180 ms.
                    delay(200);
                    Serial.printf("[sensor] BH1750 at 0x%02X\n", cand[i]);
                }
            }
        }
        if (!s_bhOk) {
            char sdaTxt[20], sclTxt[20];
            describePin(sdaTxt, sizeof(sdaTxt), s.i2cSda);
            describePin(sclTxt, sizeof(sclTxt), s.i2cScl);
            Serial.printf("[sensor] no BH1750 at 0x%02X or 0x%02X "
                          "(SDA=%s SCL=%s)\n",
                          cand[0], cand[1], sdaTxt, sclTxt);
        }
    }
    if (s_bhOk) {
        ok++;
        strncat(s_describe, s_describe[0] ? ", BH1750" : "BH1750",
                sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

#if defined(NODE_SENSOR_SDS011)
    if (!s_sdsOk) {
        s_sds.begin(9600, SWSERIAL_8N1, s.sdsRx, s.sdsTx, false);
        // No handshake to confirm: the SDS011 simply streams a frame a second
        // once powered. Treat the port as up and let readiness be decided by
        // whether a valid frame arrives before the first post.
        s_sdsOk = true;
        Serial.printf("[sensor] SDS011 listening on RX=GPIO%u TX=GPIO%u\n",
                      s.sdsRx, s.sdsTx);
    }
    if (s_sdsOk) {
        ok++;
        strncat(s_describe, s_describe[0] ? ", SDS011" : "SDS011",
                sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

#if defined(NODE_SENSOR_PULSE)
    if (!s_pulseOk) {
        pinMode(s.pulsePin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(s.pulsePin), pulseIsr, FALLING);
        s_lastDrainMs = millis();
        s_pulseOk     = true;
        Serial.printf("[sensor] pulse input on GPIO%u (%s, %g per pulse)\n",
                      s.pulsePin,
                      PULSE_MODE_RAIN ? "rain" : "flow",
                      (double)PULSE_UNITS_PER_PULSE);
    }
    if (s_pulseOk) {
        ok++;
        strncat(s_describe, s_describe[0] ? (PULSE_MODE_RAIN ? ", rain" : ", flow")
                                          : (PULSE_MODE_RAIN ? "rain"   : "flow"),
                sizeof(s_describe) - strlen(s_describe) - 1);
    }
#endif

    if (s_describe[0] == '\0') {
        strncpy(s_describe, "none", sizeof(s_describe) - 1);
        s_describe[sizeof(s_describe) - 1] = '\0';
    }
    return ok;
}

bool sensorsReady() {
    int n = 0;
#if defined(NODE_SENSOR_BMX280)
    if (s_bmxOk)      n++;
#endif
#if defined(NODE_SENSOR_BME688)
    if (s_bme688Ok)   n++;
#endif
#if defined(NODE_SENSOR_DS18B20)
    if (s_dsCount > 0) n++;
#endif
#if defined(NODE_SENSOR_BH1750)
    if (s_bhOk)       n++;
#endif
#if defined(NODE_SENSOR_SDS011)
    if (s_sdsOk)      n++;
#endif
#if defined(NODE_SENSOR_PULSE)
    if (s_pulseOk)    n++;
#endif
    return n > 0;
}

const char* sensorsDescribe() { return s_describe; }

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------
#if defined(NODE_SENSOR_DS18B20)
// "probe_temp", "probe_temp_1", ... mirroring the collector's DS18B20 plugin
// convention of a bare name for the first probe and an index suffix after.
// Storage is static because NodeReading holds the pointer until serialised.
static const char* dsMetricName(int idx) {
    static char names[DS18B20_Mini::MAX_SENSORS][20];
    if (idx <= 0) {
        snprintf(names[0], sizeof(names[0]), "%s", NODE_DS18B20_METRIC);
        return names[0];
    }
    if (idx >= DS18B20_Mini::MAX_SENSORS) idx = DS18B20_Mini::MAX_SENSORS - 1;
    snprintf(names[idx], sizeof(names[idx]), "%s_%d", NODE_DS18B20_METRIC, idx);
    return names[idx];
}
#endif

int sensorsRead(const NodeSettings& s, NodeReading* out, int maxOut) {
    int n = 0;

#if defined(NODE_SENSOR_BMX280)
    if (s_bmxOk) {
        const float t  = s_bmx.readTemperature();
        const float pa = s_bmx.readPressure();
        const float h  = s_bmx.readHumidity();       // NAN on a BMP280
        const float hPa = isfinite(pa) ? pa / 100.0f : NAN;

        add(out, n, maxOut, "temperature",  t,   "C");
        add(out, n, maxOut, "humidity",     h,   "%");
        add(out, n, maxOut, "pressure",     hPa, "hPa");
        add(out, n, maxOut, "pressure_sea",
            toSeaLevel(hPa, t, s.altitudeM), "hPa");
    }
#endif

#if defined(NODE_SENSOR_BME688)
    if (s_bme688Ok && s_bme688.performReading()) {
        const float hPa = s_bme688.pressure / 100.0f;
        add(out, n, maxOut, "temperature",    s_bme688.temperature, "C");
        add(out, n, maxOut, "humidity",       s_bme688.humidity,    "%");
        add(out, n, maxOut, "pressure",       hPa,                  "hPa");
        add(out, n, maxOut, "gas_resistance", s_bme688.gas_resistance, "Ohm");
        add(out, n, maxOut, "pressure_sea",
            toSeaLevel(hPa, s_bme688.temperature, s.altitudeM), "hPa");
    }
#endif

#if defined(NODE_SENSOR_BH1750)
    if (s_bhOk) {
        Wire.requestFrom((int)s_bhAddr, 2);
        if (Wire.available() >= 2) {
            // Two separate statements on purpose: `(read() << 8) | read()`
            // leaves the operand order unsequenced, and a right-first
            // evaluation swaps the bytes into a value that still looks like a
            // plausible lux reading.
            const uint8_t hi = (uint8_t)Wire.read();
            const uint8_t lo = (uint8_t)Wire.read();
            const uint16_t raw = ((uint16_t)hi << 8) | lo;
            add(out, n, maxOut, "lux", (float)raw / BH1750_DIVIDER, "lx");
        }
    }
#endif

#if defined(NODE_SENSOR_SDS011)
    if (s_sdsOk) {
        // Drain whatever arrived since the last post and keep the newest
        // complete frame. The sensor streams at 1 Hz and the node posts once
        // a minute, so the buffer holds many; the latest is the one to send.
        // Frame: AA C0 pm25L pm25H pm10L pm10H id1 id2 sum AB, values in
        // tenths of a ug/m3 — matching the collector's SDS011 plugin.
        static uint8_t frame[10];
        static uint8_t pos = 0;
        while (s_sds.available()) {
            const uint8_t b = (uint8_t)s_sds.read();
            if (pos == 0 && b != 0xAA) continue;
            frame[pos++] = b;
            if (pos < 10) continue;
            pos = 0;
            if (frame[1] != 0xC0 || frame[9] != 0xAB) continue;
            uint8_t sum = 0;
            for (int i = 2; i <= 7; i++) sum += frame[i];
            if (sum != frame[8]) continue;          // corrupt frame, drop it
            s_sdsPm25 = (float)((frame[3] << 8) | frame[2]) / 10.0f;
            s_sdsPm10 = (float)((frame[5] << 8) | frame[4]) / 10.0f;
        }
        add(out, n, maxOut, "pm25", s_sdsPm25, "ug/m3");
        add(out, n, maxOut, "pm10", s_sdsPm10, "ug/m3");
    }
#endif

#if defined(NODE_SENSOR_PULSE)
    if (s_pulseOk) {
        // Take and clear the count in one interrupt-off window so a pulse
        // arriving mid-read is counted exactly once — in the next interval
        // rather than neither.
        noInterrupts();
        const uint32_t pulses = s_pulses;
        s_pulses = 0;
        interrupts();

        const uint32_t nowMs   = millis();
        const uint32_t elapsed = nowMs - s_lastDrainMs;   // wrap-safe
        s_lastDrainMs = nowMs;

        const float units = (float)pulses * PULSE_UNITS_PER_PULSE;
        s_pulseTotal += units;

        // Rate as an average over the interval just ended, not extrapolated
        // from the gap between the last two pulses. For a node posting once a
        // minute the average is the honest number; extrapolation would report
        // a downpour from one tip that happened to land near the deadline.
        float rate = NAN;
        if (elapsed > 0) {
#if PULSE_MODE_RAIN
            rate = units * 3600000.0f / (float)elapsed;    // mm per hour
#else
            rate = units * 60000.0f / (float)elapsed;      // litres per minute
#endif
        }

#if PULSE_MODE_RAIN
        add(out, n, maxOut, "rain_rate",  rate,          "mm/h");
        add(out, n, maxOut, "rain_total", s_pulseTotal,  "mm");
#else
        add(out, n, maxOut, "flow_rate",  rate,          "L/min");
        add(out, n, maxOut, "flow_total", s_pulseTotal,  "L");
#endif
    }
#endif

#if defined(NODE_SENSOR_DS18B20)
    if (s_dsCount > 0) {
        s_ds.requestTemperatures();
        for (int i = 0; i < s_dsCount; i++) {
            const float t = s_ds.getTempC(i);
            // The driver reports a disconnected probe as -127, which is a
            // plausible-looking float the collector would happily store.
            if (t <= DS18B20_Mini::DISCONNECTED + 0.5f) continue;
            add(out, n, maxOut, dsMetricName(i), t, "C");
        }
    }
#endif

    return n;
}
