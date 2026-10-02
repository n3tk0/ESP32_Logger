// Host unit tests for the per-metric corrections RemoteNodeSensor applies to
// a node's readings (WiFi and ESP-NOW alike) on the collector.
//
// The trap this guards: drain() hands back the same live value again on every
// tick and a stale one keeps being repeated, so the correction must land on
// the copy, never on the mailbox, or it would compound tick after tick.
#include <stdint.h>
#include <string.h>

#define FEATURE_REMOTE_NODES 1

#include "src/sensors/RemoteIngest.cpp"
#include "src/sensors/plugins/RemoteNodeSensor.cpp"
#include "check.h"

HostSerial Serial;   // the shim declares it; RemoteNodeSensor logs through it

static const uint32_t T0 = 1750000000u;

static void initSensor(RemoteNodeSensor& s, const char* json) {
    JsonDocument doc;
    deserializeJson(doc, json);
    CHECK(s.init(doc.as<JsonObjectConst>()));
}

static float valueOf(SensorReading* out, int n, const char* metric) {
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].metric, metric) == 0) return out[i].value;
    return -9999.0f;
}

static void test_offset_and_scale_by_metric() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"out\",\"type\":\"remote\",\"calibration\":{"
                  "\"temperature\":{\"offset\":-0.5},"
                  "\"humidity\":{\"offset\":2,\"scale\":0.5}}}");
    remoteIngest.put("out", "temperature", 20.0f, "C", T0);
    remoteIngest.put("out", "humidity", 60.0f, "%", T0);
    remoteIngest.put("out", "pressure", 1000.0f, "hPa", T0);

    SensorReading out[8];
    int n = s.readAll(out, 8);
    CHECK_EQ(n, 4);                     // + the derived dew_point
    CHECK(fabsf(valueOf(out, n, "temperature") - 19.5f) < 0.001f);
    CHECK(fabsf(valueOf(out, n, "humidity") - 32.0f) < 0.001f);
    CHECK(fabsf(valueOf(out, n, "pressure") - 1000.0f) < 0.001f);   // untouched
    // From the RAW temperature and the CORRECTED humidity, as the wired
    // BME280/BME688 plugins pair them.
    CHECK(fabsf(valueOf(out, n, "dew_point") - Psychro::dewPointC(20.0f, 32.0f)) < 0.001f);

    // Same live values again: corrected once, not twice.
    n = s.readAll(out, 8);
    CHECK_EQ(n, 4);
    CHECK(fabsf(valueOf(out, n, "temperature") - 19.5f) < 0.001f);
}

static void test_backlog_is_corrected_too() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"out\",\"type\":\"remote\",\"calibration\":{"
                  "\"temperature\":{\"offset\":1}}}");
    remoteIngest.putHistorical("out", "temperature", 10.0f, "C", T0);
    remoteIngest.putHistorical("out", "temperature", 11.0f, "C", T0 + 60);

    SensorReading out[8];
    const int n = s.readAll(out, 8);
    CHECK_EQ(n, 2);
    CHECK(fabsf(out[0].value - 11.0f) < 0.001f);
    CHECK(fabsf(out[1].value - 12.0f) < 0.001f);
}

static void test_no_calibration_is_identity() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"out\",\"type\":\"remote\","
                  "\"calibration\":{\"temperature\":{\"offset\":0,\"scale\":0}}}");
    remoteIngest.put("out", "temperature", 20.0f, "C", T0);
    SensorReading out[8];
    const int n = s.readAll(out, 8);
    CHECK_EQ(n, 1);
    CHECK(fabsf(out[0].value - 20.0f) < 0.001f);
}

static int indexOf(SensorReading* out, int n, const char* metric) {
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].metric, metric) == 0) return i;
    return -1;
}

static void test_dew_point_and_iaq_are_derived() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"air\",\"type\":\"remote\"}");
    remoteIngest.put("air", "temperature", 21.0f, "C", T0);
    remoteIngest.put("air", "humidity", 40.0f, "%", T0);
    remoteIngest.put("air", "gas_resistance", 50000.0f, "Ohm", T0);
    remoteIngest.put("air", "pm25", 7.0f, "ug/m3", T0);

    SensorReading out[16];
    int n = s.readAll(out, 16);
    CHECK_EQ(n, 6);
    CHECK(fabsf(valueOf(out, n, "dew_point") - Psychro::dewPointC(21.0f, 40.0f)) < 0.001f);
    // The first reading seeds the baseline: ratio 1, humidity in the comfort
    // band, so the cleanest index there is.
    GasIaq ref;
    const float iaq0 = ref.update(40.0f, 50000.0f);
    CHECK(fabsf(valueOf(out, n, "iaq") - iaq0) < 0.001f);
    CHECK_EQ((int)out[indexOf(out, n, "dew_point")].timestamp, (int)T0);
    CHECK_STREQ(out[indexOf(out, n, "dew_point")].unit, "C");
    const char* names[16];
    CHECK_EQ(s.getMetrics(names, 16), 6);

    // The mailbox hands the same gas reading back on every tick: it is not
    // fed to the baseline again, so the index does not move.
    n = s.readAll(out, 16);
    CHECK(fabsf(valueOf(out, n, "iaq") - iaq0) < 0.001f);

    // A new, lower reading (dirtier air) moves it — by the shared formula.
    remoteIngest.put("air", "gas_resistance", 25000.0f, "Ohm", T0 + 60);
    n = s.readAll(out, 16);
    const float iaq1 = ref.update(40.0f, 25000.0f);
    CHECK(iaq1 > iaq0);
    CHECK(fabsf(valueOf(out, n, "iaq") - iaq1) < 0.001f);
}

static void test_no_humidity_no_derived() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"bmp\",\"type\":\"remote\"}");
    // A BMP280 node: temperature and pressure, no humidity.
    remoteIngest.put("bmp", "temperature", 21.0f, "C", T0);
    remoteIngest.put("bmp", "pressure", 1010.0f, "hPa", T0);
    SensorReading out[8];
    const int n = s.readAll(out, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(indexOf(out, n, "dew_point"), -1);
    CHECK_EQ(indexOf(out, n, "iaq"), -1);
}

static void test_history_goes_after_the_derived() {
    remoteIngest = RemoteIngest();
    hostSetMillis(100000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"out\",\"type\":\"remote\"}");
    remoteIngest.put("out", "temperature", 20.0f, "C", T0);
    remoteIngest.put("out", "humidity", 50.0f, "%", T0);
    for (uint32_t i = 0; i < 5; i++)
        remoteIngest.putHistorical("out", "temperature", 10.0f + i, "C", T0 - 600 + i * 60);

    // Room for the live two, the dew point and two of the five queued.
    SensorReading out[5];
    int n = s.readAll(out, 5);
    CHECK_EQ(n, 5);
    CHECK_STREQ(out[2].metric, "dew_point");
    CHECK(fabsf(out[3].value - 10.0f) < 0.001f);   // oldest history first
    CHECK(fabsf(out[4].value - 11.0f) < 0.001f);
    CHECK_EQ(remoteIngest.historyPending(), 3);

    // readLatest() is the same without the history, and takes none of it.
    n = s.readLatest(out, 5);
    CHECK_EQ(n, 3);
    CHECK_EQ(remoteIngest.historyPending(), 3);
}

int main() {
    RUN(test_offset_and_scale_by_metric);
    RUN(test_backlog_is_corrected_too);
    RUN(test_no_calibration_is_identity);
    RUN(test_dew_point_and_iaq_are_derived);
    RUN(test_no_humidity_no_derived);
    RUN(test_history_goes_after_the_derived);
    return SUMMARY();
}
