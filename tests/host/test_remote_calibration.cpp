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

// IaqBaselineStore is firmware-only (LittleFS); these stand in for it and
// record what RemoteNodeSensor asked of it.
static float       g_storedBaseline = 0.0f;
static int         g_saves = 0;
static char        g_lastPath[32];
namespace IaqBaselineStore {
void path(const char* prefix, const char* id, char* out, size_t len) {
    snprintf(out, len, "/config/%s_%s.bin", prefix, id);
}
float load(const char* p, int, int, const char*) {
    strncpy(g_lastPath, p, sizeof(g_lastPath) - 1);
    return g_storedBaseline;
}
bool save(const char* p, int, int, float baseline, uint32_t) {
    strncpy(g_lastPath, p, sizeof(g_lastPath) - 1);
    g_storedBaseline = baseline;
    g_saves++;
    return true;
}
}

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
    CHECK_EQ(n, 7);
    CHECK(fabsf(valueOf(out, n, "dew_point") - Psychro::dewPointC(21.0f, 40.0f)) < 0.001f);
    // The first reading seeds the baseline: ratio 1, humidity in the comfort
    // band, so the cleanest index there is.
    GasIaq ref;
    const float iaq0 = ref.update(40.0f, 50000.0f);
    CHECK(fabsf(valueOf(out, n, "iaq") - iaq0) < 0.001f);
    // At the baseline: the clean-air TVOC estimate.
    CHECK(fabsf(valueOf(out, n, "tvoc_est") - GasIaq::TVOC_CLEAN_PPB) < 0.001f);
    CHECK_STREQ(out[indexOf(out, n, "tvoc_est")].unit, "ppb");
    CHECK_EQ((int)out[indexOf(out, n, "tvoc_est")].timestamp, (int)T0);
    CHECK_EQ((int)out[indexOf(out, n, "dew_point")].timestamp, (int)T0);
    CHECK_STREQ(out[indexOf(out, n, "dew_point")].unit, "C");
    const char* names[16];
    CHECK_EQ(s.getMetrics(names, 16), 7);

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
    CHECK(fabsf(valueOf(out, n, "tvoc_est") - ref.tvocPpb(25000.0f)) < 0.001f);
    CHECK(valueOf(out, n, "tvoc_est") > GasIaq::TVOC_CLEAN_PPB);
}

static void test_tvoc_estimate() {
    GasIaq g;
    CHECK(isnan(g.tvocPpb(50000.0f)));                // no baseline yet
    g.baseline = 100000.0f;
    CHECK(isnan(g.tvocPpb(0.0f)));
    CHECK(fabsf(g.tvocPpb(100000.0f) - 50.0f) < 0.01f);
    CHECK(fabsf(g.tvocPpb(200000.0f) - 50.0f) < 0.01f);  // cleaner than the ceiling
    // R0/2 → 50·2^(1/0.6) ≈ 159, R0/10 → ≈ 2321, R0/100 → the cap.
    CHECK(fabsf(g.tvocPpb(50000.0f) - 158.7f) < 0.5f);
    CHECK(fabsf(g.tvocPpb(10000.0f) - 2320.8f) < 1.0f);
    CHECK(g.tvocPpb(1000.0f) == GasIaq::TVOC_MAX_PPB);
    // Humidity plays no part: only update() looks at it.
    GasIaq a, b;
    a.update(20.0f, 80000.0f); a.update(20.0f, 40000.0f);
    b.update(70.0f, 80000.0f); b.update(70.0f, 40000.0f);
    CHECK(a.tvocPpb(40000.0f) == b.tvocPpb(40000.0f));
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
    CHECK_EQ(indexOf(out, n, "tvoc_est"), -1);
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

// BME688Sensor holds the slow downward drift while its heater settles, so a
// baseline restored from flash is not dragged toward the cold, low readings.
static void test_gas_baseline_holds_while_warming() {
    GasIaq held;
    held.baseline = 80000.0f;
    held.update(40.0f, 20000.0f, false);
    CHECK(held.baseline == 80000.0f);

    // A higher reading still lifts it: that is cleaner air, not warm-up.
    held.update(40.0f, 90000.0f, false);
    CHECK(fabsf(held.baseline - 81000.0f) < 0.5f);

    // Warmed up (and by default) it drifts down again.
    GasIaq drift;
    drift.baseline = 80000.0f;
    drift.update(40.0f, 20000.0f);
    CHECK(drift.baseline < 80000.0f);
}

// A node's iaq baseline is restored on init, saved hourly, and a new
// configuration starts from the node's own file only.
static void test_node_iaq_baseline_is_kept() {
    remoteIngest = RemoteIngest();
    g_storedBaseline = 120000.0f;
    g_saves = 0;
    hostSetMillis(1000);
    RemoteNodeSensor s;
    initSensor(s, "{\"node\":\"air\",\"type\":\"remote\"}");
    CHECK_STREQ(g_lastPath, "/config/iaqn_air.bin");

    // Restored, so a first reading of 60 kOhm reads as dirtier air rather
    // than seeding a fresh baseline at 60 kOhm.
    remoteIngest.put("air", "humidity", 40.0f, "%", T0);
    remoteIngest.put("air", "gas_resistance", 60000.0f, "Ohm", T0);
    SensorReading out[16];
    int n = s.readAll(out, 16);
    GasIaq ref; ref.baseline = 120000.0f;
    CHECK(fabsf(valueOf(out, n, "iaq") - ref.update(40.0f, 60000.0f)) < 0.001f);
    s.afterRead();
    CHECK_EQ(g_saves, 0);              // not before the hour

    // An hour on, the moved baseline is written; it moved by under 2 %, so
    // only a bigger change is.
    remoteIngest.put("air", "gas_resistance", 200000.0f, "Ohm", T0 + 60);
    hostSetMillis(1000 + 3600000);
    s.readAll(out, 16);
    CHECK_EQ(g_saves, 0);              // not inside readAll (bus lock held)
    s.afterRead();
    CHECK_EQ(g_saves, 1);
    CHECK(g_storedBaseline > 120000.0f);

    // Re-initialised for another node: that node's file, nothing carried.
    g_storedBaseline = 0.0f;
    initSensor(s, "{\"node\":\"shed\",\"type\":\"remote\"}");
    CHECK_STREQ(g_lastPath, "/config/iaqn_shed.bin");
    remoteIngest.put("shed", "humidity", 40.0f, "%", T0);
    remoteIngest.put("shed", "gas_resistance", 60000.0f, "Ohm", T0);
    n = s.readAll(out, 16);
    GasIaq fresh;
    CHECK(fabsf(valueOf(out, n, "iaq") - fresh.update(40.0f, 60000.0f)) < 0.001f);
}

static void test_saver_cadence() {
    IaqBaselineStore::Saver sv;
    sv.reset(0, 100000.0f);
    CHECK(!sv.due(1000, 150000.0f));                       // within the hour
    CHECK(!sv.due(3600000, 101000.0f));                    // under 2 %
    CHECK(sv.due(7200000, 150000.0f));
    sv.done(7200000, 150000.0f, false);                    // failed: retry in 5 min
    CHECK(!sv.due(7200000 + 299000, 150000.0f));
    CHECK(sv.due(7200000 + 300000, 150000.0f));
    sv.done(7200000 + 300000, 150000.0f, true);
    CHECK(!sv.due(7200000 + 300000 + 3600000, 150500.0f)); // saved; small change
}

int main() {
    RUN(test_offset_and_scale_by_metric);
    RUN(test_backlog_is_corrected_too);
    RUN(test_no_calibration_is_identity);
    RUN(test_dew_point_and_iaq_are_derived);
    RUN(test_tvoc_estimate);
    RUN(test_no_humidity_no_derived);
    RUN(test_history_goes_after_the_derived);
    RUN(test_gas_baseline_holds_while_warming);
    RUN(test_node_iaq_baseline_is_kept);
    RUN(test_saver_cadence);
    return SUMMARY();
}
