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
    CHECK_EQ(n, 3);
    CHECK(fabsf(valueOf(out, n, "temperature") - 19.5f) < 0.001f);
    CHECK(fabsf(valueOf(out, n, "humidity") - 32.0f) < 0.001f);
    CHECK(fabsf(valueOf(out, n, "pressure") - 1000.0f) < 0.001f);   // untouched

    // Same live values again: corrected once, not twice.
    n = s.readAll(out, 8);
    CHECK_EQ(n, 3);
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

int main() {
    RUN(test_offset_and_scale_by_metric);
    RUN(test_backlog_is_corrected_too);
    RUN(test_no_calibration_is_identity);
    return SUMMARY();
}
