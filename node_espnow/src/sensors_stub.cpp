// ============================================================================
// TEMPORARY: replaced by node_common/NodeSensors.cpp at integration
// ============================================================================
//
// node_common/NodeSensors.h is the sensor layer both node firmwares share; its
// implementation is being written separately (for the ESP8266 and the
// ESP32-C3 at once). Until it lands, this file implements the same five
// functions for the one sensor this node has always had — a BME280/BMP280 on
// I2C, through the collector's src/drivers/BME280_Mini.h exactly as main.cpp
// used to — so the rest of the firmware builds and runs against the real
// interface.
//
// THE SWAP IS TWO FILE OPERATIONS AND NOTHING ELSE:
//   1. delete this file;
//   2. add node_espnow/src/node_sensors_impl.cpp containing the one line
//        #include "../../node_common/NodeSensors.cpp"
// Nothing else in node_espnow/ refers to anything defined here.
//
// Every other sensor type in the list is reported as "not answering" (no
// readings), which is what the real layer does for a sensor that is absent.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdio.h>

#include "node_common/NodeSensors.h"
#include "src/drivers/BME280_Mini.h"

using namespace nodecfg;

static BME280_Mini s_bmx;
static bool        s_bmxOk   = false;
static uint8_t     s_bmxAddr = 0;
static bool        s_any     = false;
static char        s_desc[64] = "no sensors";

/// Station pressure to sea level (hPa), the barometric formula node/ uses.
static float toSeaLevel(float stationHpa, float tempC, float altitudeM) {
    if (altitudeM == 0.0f || !isfinite(stationHpa) || !isfinite(tempC)) return NAN;
    return stationHpa * powf(1.0f - (0.0065f * altitudeM) /
                                        (tempC + 0.0065f * altitudeM + 273.15f),
                             -5.257f);
}

int nodeSensorsBegin(const NodeConfig& cfg) {
    s_bmxOk = false;
    s_any   = false;
    int up = 0;

    bool i2c = false;
    for (uint8_t i = 0; i < cfg.sensor_count; i++)
        if (sensorIsI2c(cfg.sensors[i].type)) i2c = true;
    if (i2c) Wire.begin(cfg.i2c.sda, cfg.i2c.scl);

    for (uint8_t i = 0; i < cfg.sensor_count; i++) {
        const SensorCfg& s = cfg.sensors[i];
        if (s.type != SensorType::Bmx280) continue;
        // addr 0 = probe 0x76 then 0x77, which is what this node always did.
        const uint8_t first = s.addr ? s.addr : 0x76;
        s_bmxOk = s_bmx.begin(first, &Wire);
        s_bmxAddr = first;
        if (!s_bmxOk && s.addr == 0) {
            s_bmxOk = s_bmx.begin(0x77, &Wire);
            s_bmxAddr = 0x77;
        }
        if (s_bmxOk) up++;
    }
    s_any = up > 0;
    snprintf(s_desc, sizeof(s_desc), "bmx280@0x%02X %s (stub layer)", s_bmxAddr,
             s_bmxOk ? "ok" : "absent");
    return up;
}

bool nodeSensorsReady() { return s_any; }

static void put(NodeReading* out, int& n, int maxOut, uint8_t id, float v) {
    if (n >= maxOut || !isfinite(v)) return;
    const MetricInfo* mi = metricInfo(id);
    NodeReading& r = out[n++];
    r.metricId = id;
    r.index    = 0;
    r.value    = v;
    copyStr(r.name, sizeof(r.name), mi ? mi->name : "");
    r.unit     = mi ? mi->unit : "";
}

int nodeSensorsRead(const NodeConfig& cfg, NodeReading* out, int maxOut) {
    int n = 0;
    if (!s_bmxOk || !out) return 0;
    const float t = s_bmx.readTemperature();
    // readPressure() returns PASCALS — see BME280_Mini.h.
    const float p = s_bmx.readPressure() / 100.0f;
    put(out, n, maxOut, M_TEMPERATURE, t);
    if (s_bmx.isBME280()) put(out, n, maxOut, M_HUMIDITY, s_bmx.readHumidity());
    put(out, n, maxOut, M_PRESSURE, p);
    if (cfg.altitude_m != 0.0f) put(out, n, maxOut, M_PRESSURE_SEA, toSeaLevel(p, t, cfg.altitude_m));
    return n;
}

const char* nodeSensorsDescribe() { return s_desc; }

void nodeSensorsEnd() {}
