#include "sensors.h"

#include <math.h>
#include <Wire.h>

#include "node_config.h"

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

#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688)
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

// ---------------------------------------------------------------------------
// begin
// ---------------------------------------------------------------------------
int sensorsBegin(const NodeSettings& s) {
    int ok = 0;
    s_describe[0] = '\0';

#ifdef NODE_HAS_I2C
    Wire.begin(s.i2cSda, s.i2cScl);
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
        if (!s_bmxOk) Serial.println("[sensor] no BME280/BMP280 on either address");
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
        if (!s_bme688Ok) Serial.println("[sensor] no BME680/BME688 on either address");
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
