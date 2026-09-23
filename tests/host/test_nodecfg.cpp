// Host unit tests for src/nodecfg/ — the node configuration that the WiFi
// node, the ESP-NOW node and the collector all share (docs/NODE_CONFIG.md).
//
// What is at stake is agreement. The node validates what it is sent, the
// collector validates what its UI posts, and both run the code under test
// here; a rule that is wrong is wrong on every side at once, which is better
// than the alternative, but only if the rule is right. So:
//
//   • every §1.2 rule is provoked on its own, from a config that is otherwise
//     valid, and must name the field the page highlights;
//   • every reason must reach the collector whole — CFG_ACK carries 48 bytes;
//   • the JSON codec must round-trip, keep what a partial document does not
//     mention, never hand a secret back out, and refuse a value it would
//     otherwise have to wrap (pin 262 is not GPIO6);
//   • the old flat /config.json must come across intact;
//   • the metric catalogue's order must be one both ends of the radio derive
//     the same way, because DATA2 carries ids and not names.
//
// ArduinoJson is the real library (tests/host/vendor, through the shim), so
// the decoder's edge cases are the ones the devices see.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "src/nodecfg/NodeConfigJson.h"
#include "check.h"

using namespace nodecfg;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// A WiFi node that is complete and valid: the reference node with a BME280.
static NodeConfig wifiBase() {
    NodeConfig c = configDefaults(Transport::Wifi, Hw::Esp8266);
    copyStr(c.name, sizeof(c.name), "balcony");
    copyStr(c.net.ssid, sizeof(c.net.ssid), "home");
    copyStr(c.net.pass, sizeof(c.net.pass), "correct horse");
    copyStr(c.net.host, sizeof(c.net.host), "192.168.1.50");
    copyStr(c.net.token, sizeof(c.net.token), "tok-123");
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp8266));
    return c;
}

/// An ESP-NOW node that is complete and valid: the XIAO with its BME280.
static NodeConfig espnowBase() {
    NodeConfig c = configDefaults(Transport::EspNow, Hw::Esp32c3);
    copyStr(c.name, sizeof(c.name), "garden");
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp32c3));
    return c;
}

static int g_reasonsSeen = 0;

/// The config must be refused, at `field`, with a reason containing `why`.
/// Every reason is also checked to have arrived whole: a reason of exactly 47
/// characters is what vsnprintf leaves when it had to cut, so anything
/// reaching that length is treated as truncated.
static void expectReject(const NodeConfig& c, const char* field, const char* why, int line) {
    Validation v;
    const bool ok = validate(c, v);
    ht::total()++;
    const bool good = !ok && !v.ok && strcmp(v.error.field, field) == 0 &&
                      (!why || strstr(v.error.reason, why) != nullptr) &&
                      strlen(v.error.reason) < EN_CFG_REASON_LEN - 1 &&
                      strlen(v.error.field) < EN_CFG_FIELD_LEN - 1;
    if (!good) {
        ht::failed()++;
        std::printf("  [FAIL] line %d: expected reject at \"%s\" (%s), got ok=%d \"%s\": \"%s\"\n",
                    line, field, why ? why : "", (int)ok, v.error.field, v.error.reason);
    }
    g_reasonsSeen++;
}
#define REJECT(c, field, why) expectReject((c), (field), (why), __LINE__)

static void expectAccept(const NodeConfig& c, int line) {
    Validation v;
    const bool ok = validate(c, v);
    ht::total()++;
    if (!ok) {
        ht::failed()++;
        std::printf("  [FAIL] line %d: expected accept, got \"%s\": \"%s\"\n", line,
                    v.error.field, v.error.reason);
    }
}
#define ACCEPT(c) expectAccept((c), __LINE__)

static bool hasWarning(const NodeConfig& c, const char* field, const char* why) {
    Validation v;
    validate(c, v);
    for (uint8_t i = 0; i < v.warnCount; i++)
        if (strcmp(v.warnings[i].field, field) == 0 && strstr(v.warnings[i].reason, why))
            return true;
    return false;
}

static std::string enc(const NodeConfig& c, uint8_t flags) {
    JsonDocument d;
    encodeConfig(c, d.to<JsonObject>(), flags);
    std::string s;
    serializeJson(d, s);
    return s;
}

// Two documents must serialise identically. Held in named strings: a
// CHECK_STREQ on enc(...).c_str() would compare a temporary's freed buffer.
static void sameJson(const std::string& a, const std::string& b, int line) {
    ht::total()++;
    if (a != b) {
        ht::failed()++;
        std::printf("  [FAIL] line %d:\n        got:  %s\n        want: %s\n", line,
                    a.c_str(), b.c_str());
    }
}
#define SAME_JSON(a, b) sameJson((a), (b), __LINE__)

static bool dec(const char* json, NodeConfig& c, uint8_t flags, Issue* err = nullptr) {
    JsonDocument d;
    if (deserializeJson(d, json)) return false;
    return decodeConfig(d.as<JsonVariantConst>(), c, flags, err);
}

// ===========================================================================
// Validation — §1.2, one rule at a time
// ===========================================================================

static void test_bases_are_valid() {
    ACCEPT(wifiBase());
    ACCEPT(espnowBase());
    // The defaults a firmware starts from are valid once it names itself and
    // (WiFi) has somewhere to connect: nothing in them trips a rule.
    NodeConfig e = configDefaults(Transport::EspNow, Hw::Esp32c3);
    ACCEPT(e);
}

static void test_name_rules() {
    NodeConfig c = wifiBase();
    c.name[0] = '\0';
    REJECT(c, "name", "empty");
    copyStr(c.name, sizeof(c.name), "bal cony");
    REJECT(c, "name", "letters");
    copyStr(c.name, sizeof(c.name), "a/b");
    REJECT(c, "name", "letters");
    copyStr(c.name, sizeof(c.name), "Balcony_2-east");
    ACCEPT(c);
    copyStr(c.name, sizeof(c.name), "abcdefghijklmnop");   // 16: the most
    ACCEPT(c);
}

static void test_interval_rules() {
    NodeConfig c = wifiBase();
    c.interval_s = 9;
    REJECT(c, "interval_s", "10..65535");
    c.interval_s = 10;
    ACCEPT(c);
    c.interval_s = 65535;
    ACCEPT(c);
    c.interval_s = 0;
    REJECT(c, "interval_s", nullptr);
}

static void test_altitude_and_board_rules() {
    NodeConfig c = wifiBase();
    c.altitude_m = 9001.0f;
    REJECT(c, "altitude_m", nullptr);
    c.altitude_m = NAN;
    REJECT(c, "altitude_m", nullptr);
    c.altitude_m = -20.0f;
    ACCEPT(c);
    c.board = 3;
    REJECT(c, "board", nullptr);
    c.board = 2;
    ACCEPT(c);
}

static void test_sensor_count_and_types() {
    NodeConfig c = wifiBase();
    c.sensor_count = MAX_SENSORS + 1;
    REJECT(c, "sensors", "at most 8");

    c = wifiBase();
    c.sensors[0].type = SensorType::None;
    REJECT(c, "sensors[0].type", "unknown");
    c.sensors[0].type = (SensorType)42;
    REJECT(c, "sensors[0].type", "unknown");
}

static void test_i2c_addresses() {
    NodeConfig c = wifiBase();
    c.sensors[0].addr = 0x40;
    REJECT(c, "sensors[0].addr", "0x76");
    c.sensors[0].addr = 0x77;
    ACCEPT(c);
    c.sensors[0].addr = 0;           // probe both
    ACCEPT(c);

    SensorCfg bh = sensorDefaults(SensorType::Bh1750, Hw::Esp8266);
    bh.addr = 0x76;
    addSensor(c, bh);
    REJECT(c, "sensors[1].addr", "0x23");
    c.sensors[1].addr = 0x5C;
    ACCEPT(c);
}

static void test_ds18b20_fields() {
    NodeConfig c = wifiBase();
    addSensor(c, sensorDefaults(SensorType::Ds18b20, Hw::Esp8266));
    ACCEPT(c);
    c.sensors[1].count = 0;
    REJECT(c, "sensors[1].count", "1..8");
    c.sensors[1].count = 9;
    REJECT(c, "sensors[1].count", "1..8");
    c.sensors[1].count = 1;

    c.sensors[1].metric[0] = '\0';
    REJECT(c, "sensors[1].metric", "empty");
    copyStr(c.sensors[1].metric, sizeof(c.sensors[1].metric), "Probe");
    REJECT(c, "sensors[1].metric", "lower-case");
    copyStr(c.sensors[1].metric, sizeof(c.sensors[1].metric), "1probe");
    REJECT(c, "sensors[1].metric", "lower-case");
    copyStr(c.sensors[1].metric, sizeof(c.sensors[1].metric), "pool-temp");
    REJECT(c, "sensors[1].metric", "a-z");
    copyStr(c.sensors[1].metric, sizeof(c.sensors[1].metric), "pool_temp");
    ACCEPT(c);
}

static void test_pulse_fields() {
    NodeConfig c = wifiBase();
    c.sensor_count = 0;
    addSensor(c, sensorDefaults(SensorType::Pulse, Hw::Esp8266));
    ACCEPT(c);
    c.sensors[0].per_pulse = 0.0f;
    REJECT(c, "sensors[0].per_pulse", nullptr);
    c.sensors[0].per_pulse = NAN;
    REJECT(c, "sensors[0].per_pulse", nullptr);
    c.sensors[0].per_pulse = 0.2794f;
    c.sensors[0].debounce_us = 2000000;
    REJECT(c, "sensors[0].debounce_us", nullptr);
    c.sensors[0].debounce_us = 0;
    ACCEPT(c);
    c.sensors[0].mode = (PulseMode)7;
    REJECT(c, "sensors[0].mode", nullptr);
}

static void test_sleep_unsafe_sensors() {
    // SDS011 and pulse on an ESP-NOW node that sleeps: refused, because they
    // would not fail — they would report nothing, plausibly, forever.
    NodeConfig c = espnowBase();
    addSensor(c, sensorDefaults(SensorType::Sds011, Hw::Esp32c3));
    REJECT(c, "sensors[1].type", "no sleep");
    c.sleep = false;
    ACCEPT(c);

    c = espnowBase();
    addSensor(c, sensorDefaults(SensorType::Pulse, Hw::Esp32c3));
    REJECT(c, "sensors[1].type", "no sleep");
    c.sleep = false;
    ACCEPT(c);

    // A WiFi node's `sleep` means nothing; the rule does not apply there.
    NodeConfig w = wifiBase();
    w.sleep = true;
    w.sensors[0].addr = 0;
    addSensor(w, sensorDefaults(SensorType::Sds011, Hw::Esp8266));
    ACCEPT(w);
}

static void test_one_entry_per_type() {
    NodeConfig c = wifiBase();
    addSensor(c, sensorDefaults(SensorType::Bh1750, Hw::Esp8266));
    addSensor(c, sensorDefaults(SensorType::Bh1750, Hw::Esp8266));
    REJECT(c, "sensors[2].type", "only one bh1750");

    c = wifiBase();
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp8266));
    REJECT(c, "sensors[1].type", "only one bmx280");

    // ds18b20 is the exception: one entry per bus pin, each with its own name.
    c = wifiBase();
    SensorCfg a = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    SensorCfg b = a;
    b.pin = 14;
    copyStr(b.metric, sizeof(b.metric), "pool_temp");
    addSensor(c, a);
    addSensor(c, b);
    ACCEPT(c);
}

static void test_bmx280_and_bme688_together() {
    NodeConfig c = wifiBase();
    addSensor(c, sensorDefaults(SensorType::Bme688, Hw::Esp8266));
    REJECT(c, "sensors[1].type", "same metrics");
    // Either order.
    c = wifiBase();
    c.sensors[0] = sensorDefaults(SensorType::Bme688, Hw::Esp8266);
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp8266));
    REJECT(c, "sensors[1].type", "same metrics");
}

static void test_metric_budget() {
    // bme688 (5) + sds011 (2) + bh1750 (1) = 8: exactly the budget.
    NodeConfig c = wifiBase();
    c.sensors[0] = sensorDefaults(SensorType::Bme688, Hw::Esp8266);
    addSensor(c, sensorDefaults(SensorType::Sds011, Hw::Esp8266));
    addSensor(c, sensorDefaults(SensorType::Bh1750, Hw::Esp8266));
    CHECK_EQ((int)configMetricCount(c), 8);
    ACCEPT(c);
    // One probe more is nine.
    addSensor(c, sensorDefaults(SensorType::Ds18b20, Hw::Esp8266));
    CHECK_EQ((int)configMetricCount(c), 9);
    REJECT(c, "sensors", "9 metrics");

    // pressure_sea counts even at altitude 0 (§1.1): bmx280 is 4, and 4 probes
    // make 8 whether or not the altitude is ever set.
    c = wifiBase();
    c.altitude_m = 0.0f;
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    d.count = 4;
    addSensor(c, d);
    ACCEPT(c);
    c.sensors[1].count = 5;
    REJECT(c, "sensors", "at most 8");

    // Eight probes alone are fine.
    c = wifiBase();
    c.sensor_count = 0;
    d.count = 8;
    addSensor(c, d);
    ACCEPT(c);
}

static void test_metric_names_unique() {
    // A ds18b20 named "temperature" beside a BME280 would land on the same
    // (node, metric) key on the collector and overwrite it every tick.
    // ("temperature" itself is 11 characters, one over the metric limit, so
    // the collision is shown with "humidity", which fits.)
    NodeConfig c = wifiBase();
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    copyStr(d.metric, sizeof(d.metric), "humidity");
    addSensor(c, d);
    REJECT(c, "sensors[1].metric", "metric humidity is already in use");

    // Two ds18b20 buses with the default name collide on probe_temp.
    c = wifiBase();
    d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    SensorCfg e = d;
    e.pin = 14;
    addSensor(c, d);
    addSensor(c, e);
    REJECT(c, "sensors[2].metric", "probe_temp");

    // …and a bus whose plain name equals another bus's suffixed probe: "pool"
    // with two probes publishes pool and pool_1, so a bus called pool_1
    // collides with its second probe.
    c = wifiBase();
    copyStr(d.metric, sizeof(d.metric), "pool_1");
    copyStr(e.metric, sizeof(e.metric), "pool");
    e.count = 2;
    addSensor(c, d);
    addSensor(c, e);
    REJECT(c, "sensors[2].metric", "metric pool_1 is already in use");
    copyStr(c.sensors[2].metric, sizeof(c.sensors[2].metric), "tank");
    ACCEPT(c);
}

static void test_forbidden_and_missing_pins() {
    NodeConfig c = wifiBase();
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    d.pin = 6;
    addSensor(c, d);
    REJECT(c, "sensors[1].pin", "GPIO6 is the SPI flash bus");
    for (uint8_t g = 7; g <= 11; g++) {
        c.sensors[1].pin = g;
        REJECT(c, "sensors[1].pin", "flash");
    }
    c.sensors[1].pin = 17;
    REJECT(c, "sensors[1].pin", "not a pin on the esp8266");
    c.sensors[1].pin = 14;
    ACCEPT(c);

    c.i2c.sda = 9;
    REJECT(c, "i2c.sda", "flash");

    NodeConfig e = espnowBase();
    for (uint8_t g = 12; g <= 17; g++) {
        e.i2c.scl = g;
        REJECT(e, "i2c.scl", "flash");
    }
    e.i2c.scl = 22;
    REJECT(e, "i2c.scl", "not a pin on the esp32c3");
}

static void test_pins_used_twice() {
    // The ESP8266 defaults put pulse on GPIO4, which is also I2C SDA — the
    // clash node_config.h documents. With an I2C sensor present it is refused
    // and names the field that had the pin first.
    NodeConfig c = wifiBase();
    addSensor(c, sensorDefaults(SensorType::Pulse, Hw::Esp8266));
    REJECT(c, "sensors[1].pin", "already used by i2c.sda");

    // Without an I2C sensor the I2C pins are not in use, so the same pin is fine.
    c.sensors[0] = c.sensors[1];
    c.sensor_count = 1;
    ACCEPT(c);

    // SDA and SCL on one pin.
    c = wifiBase();
    c.i2c.scl = c.i2c.sda;
    REJECT(c, "i2c.scl", "already used by i2c.sda");

    // SDS011 RX and TX on one pin.
    c = wifiBase();
    SensorCfg s = sensorDefaults(SensorType::Sds011, Hw::Esp8266);
    s.tx = s.rx;
    addSensor(c, s);
    REJECT(c, "sensors[1].tx", "already used by sensors[1].rx");

    // The battery pin counts on an ESP-NOW node.
    NodeConfig e = espnowBase();
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    d.pin = e.batt.pin;
    addSensor(e, d);
    REJECT(e, "batt.pin", "already used by sensors[1].pin");
}

static void test_interrupt_pins() {
    // GPIO16 on the ESP8266 has no interrupt: SoftwareSerial RX and the pulse
    // counter both depend on one.
    NodeConfig c = wifiBase();
    SensorCfg s = sensorDefaults(SensorType::Sds011, Hw::Esp8266);
    s.rx = 16;
    addSensor(c, s);
    REJECT(c, "sensors[1].rx", "no interrupt");
    c.sensors[1].rx = 14;
    c.sensors[1].tx = 16;      // TX does not need one
    ACCEPT(c);

    c = wifiBase();
    SensorCfg p = sensorDefaults(SensorType::Pulse, Hw::Esp8266);
    p.pin = 16;
    addSensor(c, p);
    REJECT(c, "sensors[1].pin", "no interrupt");
}

static void test_battery_pin_must_be_adc() {
    NodeConfig e = espnowBase();
    e.batt.pin = 5;            // ADC2 — the radio owns it
    REJECT(e, "batt.pin", "ADC1");
    e.batt.pin = 3;
    ACCEPT(e);
    // A WiFi node has no battery section at all; its batt.pin is not checked.
    NodeConfig w = wifiBase();
    w.batt.pin = 7;
    ACCEPT(w);
}

static void test_pin_warnings() {
    // GPIO0 on the ESP8266: accepted, with the strap warning attached.
    NodeConfig c = wifiBase();
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    d.pin = 0;
    addSensor(c, d);
    ACCEPT(c);
    CHECK(hasWarning(c, "sensors[1].pin", "GPIO0: boot strap"));

    // The C3's straps and USB pins, and the XIAO's A0 battery default.
    NodeConfig e = espnowBase();
    e.i2c.sda = 8;
    e.i2c.scl = 9;
    ACCEPT(e);
    CHECK(hasWarning(e, "i2c.sda", "GPIO8"));
    CHECK(hasWarning(e, "i2c.scl", "BOOT"));
    CHECK(hasWarning(e, "batt.pin", "GPIO2"));
    e.i2c.sda = 18;
    e.i2c.scl = 19;
    CHECK(hasWarning(e, "i2c.sda", "USB"));

    // Free pins warn about nothing.
    Validation v;
    validate(espnowBase(), v);
    CHECK_EQ((int)v.warnCount, 1);   // only batt.pin = GPIO2
    // The warning reasons fit too.
    for (uint8_t i = 0; i < v.warnCount; i++)
        CHECK(strlen(v.warnings[i].reason) < EN_CFG_REASON_LEN - 1);
}

static void test_battery_and_link_ranges() {
    NodeConfig e = espnowBase();
    e.batt.divider = 0.5f;
    REJECT(e, "batt.divider", nullptr);
    e.batt.divider = NAN;
    REJECT(e, "batt.divider", nullptr);
    e.batt.divider = 2.0f;
    e.batt.trim = 2.0f;
    REJECT(e, "batt.trim", nullptr);
    e.batt.trim = 1.0f;
    e.link.ack_window_ms = 1;
    REJECT(e, "link.ack_window_ms", nullptr);
    e.link.ack_window_ms = 30;
    e.link.rescan_fails = 0;
    REJECT(e, "link.rescan_fails", nullptr);
}

static void test_net_rules() {
    NodeConfig c = wifiBase();
    c.net.ssid[0] = '\0';
    REJECT(c, "net.ssid", nullptr);
    c = wifiBase();
    c.net.host[0] = '\0';
    REJECT(c, "net.host", nullptr);
    c = wifiBase();
    c.net.port = 0;
    REJECT(c, "net.port", nullptr);
    c = wifiBase();
    copyStr(c.net.pass, sizeof(c.net.pass), "short");
    REJECT(c, "net.pass", "8..63");
    c.net.pass[0] = '\0';             // an open network
    ACCEPT(c);
    copyStr(c.net.next.pass, sizeof(c.net.next.pass), "1234567");
    REJECT(c, "net.next.pass", "8..63");

    // An ESP-NOW node has no `net`; an empty one is not an error there.
    ACCEPT(espnowBase());
}

static void test_lmk_rule() {
    NodeConfig e = espnowBase();
    copyStr(e.lmk, sizeof(e.lmk), "short");
    REJECT(e, "lmk", "16");
    copyStr(e.lmk, sizeof(e.lmk), "0123456789abcdef");
    ACCEPT(e);
}

static void test_validation_json_shape() {
    NodeConfig c = wifiBase();
    c.interval_s = 1;
    Validation v;
    validate(c, v);
    JsonDocument d;
    encodeValidation(v, d.to<JsonObject>());
    std::string s;
    serializeJson(d, s);
    CHECK_STREQ(s.c_str(),
                "{\"ok\":false,\"field\":\"interval_s\",\"reason\":\"must be 10..65535 seconds\"}");

    validate(wifiBase(), v);
    JsonDocument d2;
    encodeValidation(v, d2.to<JsonObject>());
    s.clear();
    serializeJson(d2, s);
    CHECK_STREQ(s.c_str(), "{\"ok\":true}");

    // And a rejection travels in a CFG_ACK without losing a byte.
    CfgAckMsg a;
    validate(c, v);
    espnowFillCfgAck(a, 1, 5, EN_CFG_REJECTED, v.error.field, v.error.reason);
    CHECK_STREQ(a.field, v.error.field);
    CHECK_STREQ(a.reason, v.error.reason);
}

// ===========================================================================
// Metric catalogue
// ===========================================================================

static void test_catalogue_ids() {
    // §5, in order. These are wire values.
    const char* want[] = { "", "temperature", "humidity", "pressure", "pressure_sea",
                           "gas_resistance", "lux", "pm25", "pm10", "rain_rate",
                           "rain_total", "flow_rate", "flow_total", "probe_temp",
                           "battery_voltage" };
    CHECK_EQ((int)METRIC_ID_MAX, 14);
    for (uint8_t i = 1; i <= METRIC_ID_MAX; i++) {
        CHECK_EQ((int)METRICS[i].id, (int)i);
        CHECK_STREQ(metricInfo(i)->name, want[i]);
        CHECK_EQ((int)metricIdByName(want[i]), (int)i);
        // SensorReading::metric is char[16] on the collector.
        CHECK(strlen(METRICS[i].name) <= 15);
    }
    CHECK(metricInfo(0) == nullptr);
    CHECK(metricInfo(15) == nullptr);
    CHECK_EQ((int)metricIdByName("nonsense"), 0);
    // Units the collector's own plugins use, so a remote reading is
    // indistinguishable from a wired one.
    CHECK_STREQ(metricInfo(M_TEMPERATURE)->unit, "C");
    CHECK_STREQ(metricInfo(M_PRESSURE)->unit, "hPa");
    CHECK_STREQ(metricInfo(M_GAS_RESISTANCE)->unit, "Ohm");
    CHECK_STREQ(metricInfo(M_LUX)->unit, "lx");
    CHECK_STREQ(metricInfo(M_PM25)->unit, "ug/m3");
    CHECK_STREQ(metricInfo(M_BATTERY_VOLTAGE)->unit, "V");
}

static void test_counts_per_type() {
    // §1.1's Count column.
    CHECK_EQ((int)sensorMetricCount(sensorDefaults(SensorType::Bmx280, Hw::Esp8266)), 4);
    CHECK_EQ((int)sensorMetricCount(sensorDefaults(SensorType::Bme688, Hw::Esp8266)), 5);
    CHECK_EQ((int)sensorMetricCount(sensorDefaults(SensorType::Bh1750, Hw::Esp8266)), 1);
    CHECK_EQ((int)sensorMetricCount(sensorDefaults(SensorType::Sds011, Hw::Esp8266)), 2);
    CHECK_EQ((int)sensorMetricCount(sensorDefaults(SensorType::Pulse, Hw::Esp8266)), 2);
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    d.count = 5;
    CHECK_EQ((int)sensorMetricCount(d), 5);
}

static void test_list_metrics_order_and_names() {
    NodeConfig c = espnowBase();                 // bmx280
    c.sleep = false;
    SensorCfg a = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    a.count = 2;
    SensorCfg b = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    b.pin = 5;
    copyStr(b.metric, sizeof(b.metric), "pool");
    SensorCfg p = sensorDefaults(SensorType::Pulse, Hw::Esp32c3);
    pulseModeDefaults(p, PulseMode::Flow);
    p.pin = 10;
    // bmx280 (4) + a (2) + b (1) + pulse (2) = 9: over budget, but listing
    // does not judge — it must still list, in order.
    addSensor(c, a);
    addSensor(c, b);
    addSensor(c, p);

    MetricSlot s[16];
    const uint8_t n = listMetrics(c, s, 16, true);
    CHECK_EQ((int)n, 10);
    const char* names[] = { "temperature", "humidity", "pressure", "pressure_sea",
                            "probe_temp", "probe_temp_1", "pool", "flow_rate", "flow_total",
                            "battery_voltage" };
    for (uint8_t i = 0; i < n; i++) CHECK_STREQ(s[i].name, names[i]);
    CHECK(s[3].needsAltitude);
    CHECK(!s[0].needsAltitude);
    // Probe ordinals run across buses: a's are 0 and 1, b's is 2.
    CHECK_EQ((int)s[4].index, 0);
    CHECK_EQ((int)s[5].index, 1);
    CHECK_EQ((int)s[6].index, 2);
    CHECK_EQ((int)s[6].sensor, 2);
    CHECK_EQ((int)s[6].probe, 0);
    CHECK_EQ((int)s[9].id, (int)M_BATTERY_VOLTAGE);
    CHECK_STREQ(s[7].unit, "L/min");

    // A short array still gets the true total back.
    CHECK_EQ((int)listMetrics(c, s, 3, false), 9);
    CHECK_EQ((int)listMetrics(c, nullptr, 0, false), 9);

    // The collector's reverse lookup agrees with the node's forward one for
    // every probe slot — this is the DATA2 contract.
    for (uint8_t i = 0; i < n; i++) {
        char name[METRIC_NAME_CAP];
        const char* unit = metricNameFor(&c, s[i].id, s[i].index, name);
        CHECK(unit != nullptr);
        CHECK_STREQ(name, s[i].name);
    }
    char name[METRIC_NAME_CAP];
    CHECK(metricNameFor(&c, M_PROBE_TEMP, 3, name) == nullptr);   // no fourth probe
    CHECK(metricNameFor(&c, 99, 0, name) == nullptr);             // unknown id

    // Without a reported config: the default naming, right for the common node.
    CHECK(metricNameFor(nullptr, M_PROBE_TEMP, 0, name) != nullptr);
    CHECK_STREQ(name, "probe_temp");
    metricNameFor(nullptr, M_PROBE_TEMP, 3, name);
    CHECK_STREQ(name, "probe_temp_3");
    metricNameFor(nullptr, M_LUX, 0, name);
    CHECK_STREQ(name, "lux");
}

// ===========================================================================
// JSON
// ===========================================================================

static NodeConfig wifiFull() {
    NodeConfig c = wifiBase();
    c.rev = 4;
    c.local = true;
    copyStr(c.fw, sizeof(c.fw), "2026.09.1");
    c.interval_s = 300;
    c.altitude_m = 512.5f;
    c.board = 1;
    c.sensors[0].addr = 0x77;
    SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp8266);
    d.count = 2;
    copyStr(d.metric, sizeof(d.metric), "soil");
    addSensor(c, d);
    SensorCfg p = sensorDefaults(SensorType::Pulse, Hw::Esp8266);
    p.pin = 13;
    addSensor(c, p);
    copyStr(c.net.basic_user, sizeof(c.net.basic_user), "admin");
    copyStr(c.net.basic_pass, sizeof(c.net.basic_pass), "letmein!");
    c.net.port = 8080;
    copyStr(c.net.next.ssid, sizeof(c.net.next.ssid), "home-5g");
    copyStr(c.net.next.pass, sizeof(c.net.next.pass), "another passphrase");
    return c;
}

static NodeConfig espnowFull() {
    NodeConfig c = espnowBase();
    c.rev = 9;
    c.sleep = false;
    c.board = 1;
    c.interval_s = 120;
    SensorCfg s = sensorDefaults(SensorType::Sds011, Hw::Esp32c3);
    addSensor(c, s);
    SensorCfg b = sensorDefaults(SensorType::Bh1750, Hw::Esp32c3);
    b.addr = 0x5C;
    addSensor(c, b);
    c.link.ack_window_ms = 50;
    c.link.rescan_fails = 5;
    c.link.rescan_min_s = 7200;
    copyStr(c.link.next_ssid, sizeof(c.link.next_ssid), "new-net");
    c.batt.pin = 3;
    c.batt.divider = 3.0f;
    c.batt.trim = 1.02f;
    return c;
}

static void test_round_trip() {
    // With secrets, decoded with the identity and rev flags: what the node
    // writes to /config.json and reads back on the next boot.
    const NodeConfig w = wifiFull();
    ACCEPT(w);
    const std::string js = enc(w, NCJ_SECRETS);
    NodeConfig back = configDefaults(Transport::Wifi, Hw::Esp8266);
    CHECK(dec(js.c_str(), back, NCJ_DEC_REV | NCJ_DEC_IDENTITY));
    SAME_JSON(enc(back, NCJ_SECRETS), js);
    CHECK_EQ((int)back.sensor_count, 3);
    CHECK_EQ((int)back.sensors[1].count, 2);
    CHECK_STREQ(back.sensors[1].metric, "soil");
    CHECK_STREQ(back.net.basic_pass, "letmein!");
    CHECK_STREQ(back.net.next.pass, "another passphrase");
    CHECK(back.altitude_m == 512.5f);
    CHECK(back.local);

    const NodeConfig e = espnowFull();
    ACCEPT(e);
    const std::string je = enc(e, 0);
    NodeConfig eb = configDefaults(Transport::EspNow, Hw::Esp32c3);
    CHECK(dec(je.c_str(), eb, NCJ_DEC_REV | NCJ_DEC_IDENTITY));
    SAME_JSON(enc(eb, 0), je);
    CHECK_EQ((int)eb.link.rescan_min_s, 7200);
    CHECK(eb.batt.trim > 1.019f && eb.batt.trim < 1.021f);
    CHECK_EQ((int)eb.sensors[2].addr, 0x5C);
}

static void test_sections_follow_transport() {
    JsonDocument d;
    deserializeJson(d, enc(wifiFull(), 0));
    CHECK(!d["net"].isNull());
    CHECK(d["link"].isNull());
    CHECK(d["batt"].isNull());
    CHECK(d["sleep"].isNull());
    CHECK(d["lmk"].isNull());

    NodeConfig e = espnowFull();
    copyStr(e.lmk, sizeof(e.lmk), "0123456789abcdef");
    JsonDocument d2;
    deserializeJson(d2, enc(e, 0));
    CHECK(d2["net"].isNull());          // the radio's CFG: no net, ever
    CHECK(!d2["link"].isNull());
    CHECK(!d2["batt"].isNull());
    CHECK(d2["sleep"].is<bool>());
    CHECK(d2["lmk"].isNull());          // not without NCJ_LMK

    // With NCJ_LMK — and even with NCJ_SECRETS — the key itself never appears.
    const std::string withLmk = enc(e, NCJ_LMK | NCJ_SECRETS);
    CHECK(withLmk.find("0123456789abcdef") == std::string::npos);
    JsonDocument d3;
    deserializeJson(d3, withLmk);
    CHECK_STREQ(d3["lmk"].as<const char*>(), "");
    CHECK(d3["lmk_set"].as<bool>());
}

static void test_secrets_are_blanked() {
    NodeConfig w = wifiFull();
    w.net.token[0] = '\0';
    const std::string s = enc(w, 0);
    CHECK(s.find("correct horse") == std::string::npos);
    CHECK(s.find("letmein!") == std::string::npos);
    CHECK(s.find("another passphrase") == std::string::npos);

    JsonDocument d;
    deserializeJson(d, s);
    CHECK_STREQ(d["net"]["pass"].as<const char*>(), "");
    CHECK(d["net"]["pass_set"].as<bool>());
    CHECK_STREQ(d["net"]["token"].as<const char*>(), "");
    CHECK(!d["net"]["token_set"].as<bool>());
    CHECK(d["net"]["basic_pass_set"].as<bool>());
    CHECK(d["net"]["next"]["pass_set"].as<bool>());
    // Non-secret neighbours are there.
    CHECK_STREQ(d["net"]["ssid"].as<const char*>(), "home");
    CHECK_STREQ(d["net"]["basic_user"].as<const char*>(), "admin");

    // With secrets: values AND flags, so "" plus false means "none".
    JsonDocument d2;
    deserializeJson(d2, enc(w, NCJ_SECRETS));
    CHECK_STREQ(d2["net"]["pass"].as<const char*>(), "correct horse");
    CHECK_STREQ(d2["net"]["token"].as<const char*>(), "");
    CHECK(d2["net"]["token_set"].is<bool>());
    CHECK(!d2["net"]["token_set"].as<bool>());
}

static void test_secret_decode_rules() {
    NodeConfig c = wifiFull();
    // "" keeps.
    CHECK(dec("{\"net\":{\"pass\":\"\",\"token\":\"\"}}", c, 0));
    CHECK_STREQ(c.net.pass, "correct horse");
    CHECK_STREQ(c.net.token, "tok-123");
    // "" with _set:true keeps (a GET echoed back).
    CHECK(dec("{\"net\":{\"pass\":\"\",\"pass_set\":true}}", c, 0));
    CHECK_STREQ(c.net.pass, "correct horse");
    // A value sets.
    CHECK(dec("{\"net\":{\"pass\":\"brand new pass\"}}", c, 0));
    CHECK_STREQ(c.net.pass, "brand new pass");
    // "" with _set:false clears — the only way to remove an optional secret.
    CHECK(dec("{\"net\":{\"basic_pass\":\"\",\"basic_pass_set\":false}}", c, 0));
    CHECK_STREQ(c.net.basic_pass, "");
    // _set:false WITHOUT the key present does nothing.
    CHECK(dec("{\"net\":{\"token_set\":false}}", c, 0));
    CHECK_STREQ(c.net.token, "tok-123");

    // Echoing a secret-blanked GET back changes nothing at all.
    const NodeConfig before = wifiFull();
    NodeConfig echo = before;
    CHECK(dec(enc(before, 0).c_str(), echo, 0));
    SAME_JSON(enc(echo, NCJ_SECRETS), enc(before, NCJ_SECRETS));

    // The collector merging a node's report into its desired config keeps the
    // secrets it holds (the report has them blank).
    NodeConfig desired = wifiFull();
    NodeConfig reported = wifiFull();
    reported.interval_s = 900;
    CHECK(dec(enc(reported, 0).c_str(), desired, NCJ_DEC_REV | NCJ_DEC_IDENTITY));
    CHECK_EQ((int)desired.interval_s, 900);
    CHECK_STREQ(desired.net.pass, "correct horse");
}

static void test_partial_decode() {
    const NodeConfig before = wifiFull();
    NodeConfig c = before;
    CHECK(dec("{\"interval_s\":120}", c, 0));
    CHECK_EQ((int)c.interval_s, 120);
    c.interval_s = before.interval_s;
    SAME_JSON(enc(c, NCJ_SECRETS), enc(before, NCJ_SECRETS));

    // Unknown keys are ignored (forward compatibility), and so is null.
    c = before;
    CHECK(dec("{\"future_key\":{\"x\":1},\"name\":null,\"i2c\":{\"sda\":12}}", c, 0));
    CHECK_STREQ(c.name, "balcony");
    CHECK_EQ((int)c.i2c.sda, 12);
    CHECK_EQ((int)c.i2c.scl, 5);

    // rev/local only with NCJ_DEC_REV; identity only with NCJ_DEC_IDENTITY.
    c = before;
    CHECK(dec("{\"rev\":99,\"local\":false,\"transport\":\"espnow\",\"hw\":\"esp32c3\","
              "\"fw\":\"x\"}", c, 0));
    CHECK_EQ((int)c.rev, 4);
    CHECK(c.local);
    CHECK(c.transport == Transport::Wifi);
    CHECK(c.hw == Hw::Esp8266);
    CHECK_STREQ(c.fw, "2026.09.1");
    CHECK(dec("{\"rev\":99,\"local\":false}", c, NCJ_DEC_REV));
    CHECK_EQ((int)c.rev, 99);
    CHECK(!c.local);
    CHECK(dec("{\"hw\":\"esp32c3\",\"fw\":\"x\"}", c, NCJ_DEC_IDENTITY));
    CHECK(c.hw == Hw::Esp32c3);
    CHECK_STREQ(c.fw, "x");

    // A `net` sent to an ESP-NOW node is ignored, not stored.
    NodeConfig e = espnowFull();
    CHECK(dec("{\"net\":{\"ssid\":\"x\",\"pass\":\"yyyyyyyyyy\"}}", e, 0));
    CHECK_STREQ(e.net.ssid, "");
    CHECK_STREQ(e.net.pass, "");
    // …and link/batt sent to a WiFi node likewise.
    c = before;
    CHECK(dec("{\"batt\":{\"pin\":3}}", c, 0));
    CHECK_EQ((int)c.batt.pin, (int)BattCfg().pin);
}

static void test_sensor_list_decode() {
    NodeConfig c = wifiFull();   // bmx280 0x77, ds18b20 soil×2, pulse pin 13
    // Same position, same type: unmentioned fields are kept.
    CHECK(dec("{\"sensors\":[{\"type\":\"bmx280\"},{\"type\":\"ds18b20\",\"count\":3}]}", c, 0));
    CHECK_EQ((int)c.sensor_count, 2);                   // the list is replaced
    CHECK_EQ((int)c.sensors[0].addr, 0x77);
    CHECK_EQ((int)c.sensors[1].count, 3);
    CHECK_STREQ(c.sensors[1].metric, "soil");
    CHECK(c.sensors[2].type == SensorType::None);       // the old third slot is cleared

    // A different type in a slot starts from that type's defaults.
    CHECK(dec("{\"sensors\":[{\"type\":\"bh1750\"}]}", c, 0));
    CHECK_EQ((int)c.sensors[0].addr, 0x23);

    // Changing a pulse counter's mode brings the mode's scale and debounce.
    c = wifiFull();
    CHECK(dec("{\"sensors\":[{\"type\":\"bmx280\"},{\"type\":\"ds18b20\"},"
              "{\"type\":\"pulse\",\"mode\":\"flow\"}]}", c, 0));
    CHECK(c.sensors[2].mode == PulseMode::Flow);
    CHECK_EQ((int)c.sensors[2].debounce_us, 0);
    CHECK(c.sensors[2].per_pulse < 0.01f);
    CHECK_EQ((int)c.sensors[2].pin, 13);                // kept
    // …unless the document gives them.
    CHECK(dec("{\"sensors\":[{\"type\":\"bmx280\"},{\"type\":\"ds18b20\"},"
              "{\"type\":\"pulse\",\"mode\":\"rain\",\"debounce_us\":5000}]}", c, 0));
    CHECK_EQ((int)c.sensors[2].debounce_us, 5000);

    // An empty list is a valid list.
    CHECK(dec("{\"sensors\":[]}", c, 0));
    CHECK_EQ((int)c.sensor_count, 0);
}

static void expectDecodeError(const char* json, const char* field, const char* why, int line) {
    NodeConfig c = wifiFull();
    const std::string before = enc(c, NCJ_SECRETS);
    Issue err;
    const bool ok = dec(json, c, NCJ_DEC_REV | NCJ_DEC_IDENTITY, &err);
    ht::total()++;
    const bool good = !ok && strcmp(err.field, field) == 0 && strstr(err.reason, why) &&
                      enc(c, NCJ_SECRETS) == before;   // all-or-nothing
    if (!good) {
        ht::failed()++;
        std::printf("  [FAIL] line %d: %s -> expected \"%s\" (%s), got ok=%d \"%s\": \"%s\"\n",
                    line, json, field, why, (int)ok, err.field, err.reason);
    }
}
#define DECODE_ERROR(json, field, why) expectDecodeError((json), (field), (why), __LINE__)

static void test_decode_errors() {
    DECODE_ERROR("{\"interval_s\":\"abc\"}", "interval_s", "must be a number");
    DECODE_ERROR("{\"interval_s\":60.5}", "interval_s", "whole number");
    DECODE_ERROR("{\"interval_s\":70000}", "interval_s", "0..65535");
    DECODE_ERROR("{\"interval_s\":-1}", "interval_s", "0..65535");
    // Out of a pin's range is refused, never wrapped: 262 & 0xFF is GPIO6.
    DECODE_ERROR("{\"i2c\":{\"sda\":262}}", "i2c.sda", "0..255");
    DECODE_ERROR("{\"sensors\":[{\"type\":\"ds18b20\",\"pin\":300}]}", "sensors[0].pin", "0..255");
    DECODE_ERROR("{\"sensors\":[{\"type\":\"bmx280\"},{\"type\":\"thermistor\"}]}",
                 "sensors[1].type", "unknown");
    DECODE_ERROR("{\"sensors\":[{\"addr\":118}]}", "sensors[0].type", "unknown");
    DECODE_ERROR("{\"sensors\":[5]}", "sensors[0]", "object");
    DECODE_ERROR("{\"sensors\":{}}", "sensors", "array");
    DECODE_ERROR("{\"sensors\":[{\"type\":\"bh1750\"},{\"type\":\"bh1750\"},{\"type\":\"bh1750\"},"
                 "{\"type\":\"bh1750\"},{\"type\":\"bh1750\"},{\"type\":\"bh1750\"},"
                 "{\"type\":\"bh1750\"},{\"type\":\"bh1750\"},{\"type\":\"bh1750\"}]}",
                 "sensors", "at most 8");
    DECODE_ERROR("{\"sensors\":[{\"type\":\"pulse\",\"mode\":\"wind\"}]}", "sensors[0].mode",
                 "rain or flow");
    DECODE_ERROR("{\"sensors\":[{\"type\":\"ds18b20\",\"metric\":\"temperature\"}]}",
                 "sensors[0].metric", "at most 10");
    DECODE_ERROR("{\"name\":\"abcdefghijklmnopq\"}", "name", "at most 16");
    DECODE_ERROR("{\"name\":7}", "name", "string");
    DECODE_ERROR("{\"altitude_m\":\"high\"}", "altitude_m", "number");
    DECODE_ERROR("{\"local\":1}", "local", "true or false");
    DECODE_ERROR("{\"transport\":\"lora\"}", "transport", "wifi or espnow");
    DECODE_ERROR("{\"hw\":\"esp32\"}", "hw", "esp8266 or esp32c3");
    DECODE_ERROR("{\"net\":{\"port\":65536}}", "net.port", "0..65535");
    DECODE_ERROR("{\"net\":{\"pass\":5}}", "net.pass", "string");
    DECODE_ERROR("{\"net\":{\"next\":{\"ssid\":\"012345678901234567890123456789012\"}}}",
                 "net.next.ssid", "at most 32");
    DECODE_ERROR("{\"net\":[]}", "net", "object");
    DECODE_ERROR("{\"i2c\":4}", "i2c", "object");
    DECODE_ERROR("[1,2]", "", "object");
}

static void test_lmk_decode() {
    NodeConfig e = espnowFull();
    // Only with NCJ_DEC_LMK: the collector never accepts a key.
    CHECK(dec("{\"lmk\":\"0123456789abcdef\"}", e, 0));
    CHECK_STREQ(e.lmk, "");
    CHECK(dec("{\"lmk\":\"0123456789abcdef\"}", e, NCJ_DEC_LMK));
    CHECK_STREQ(e.lmk, "0123456789abcdef");
    // "" keeps; "" + lmk_set:false goes back to the compiled key.
    CHECK(dec("{\"lmk\":\"\"}", e, NCJ_DEC_LMK));
    CHECK_STREQ(e.lmk, "0123456789abcdef");
    CHECK(dec("{\"lmk\":\"\",\"lmk_set\":false}", e, NCJ_DEC_LMK));
    CHECK_STREQ(e.lmk, "");

    Issue err;
    CHECK(!dec("{\"lmk\":\"short\"}", e, NCJ_DEC_LMK, &err));
    CHECK_STREQ(err.field, "lmk");
    CHECK(!dec("{\"lmk\":\"0123456789abcdef0\"}", e, NCJ_DEC_LMK, &err));
    CHECK_STREQ(err.field, "lmk");
}

static void test_documents_fit_their_transports() {
    // ESP-NOW: the biggest config the validator can accept must fit
    // EN_CFG_MAX_TOTAL, or a node could be told something it cannot be sent.
    // Eight ds18b20 entries with ten-character names is the widest list.
    NodeConfig e = espnowFull();
    e.sensor_count = 0;
    const uint8_t pins[8] = { 0, 1, 2, 4, 5, 6, 7, 10 };   // batt.pin is 3
    for (uint8_t i = 0; i < 8; i++) {
        SensorCfg d = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
        d.pin = pins[i];
        char m[11] = "probe0000X";                // ten characters, the most
        m[9] = (char)('0' + i);
        copyStr(d.metric, sizeof(d.metric), m);
        addSensor(e, d);
    }
    copyStr(e.name, sizeof(e.name), "abcdefghijklmnop");
    copyStr(e.fw, sizeof(e.fw), "2026.09.10-rc1-dirty-x");
    copyStr(e.link.next_ssid, sizeof(e.link.next_ssid), "0123456789012345678901234567890");
    e.altitude_m = -123.456f;
    e.rev = 65535;
    ACCEPT(e);
    char buf[EN_CFG_MAX_TOTAL + 1];
    const size_t n = encodeConfigTo(e, buf, sizeof(buf), 0);
    CHECK(n > 0);
    CHECK(n <= EN_CFG_MAX_TOTAL);
    std::printf("       widest ESP-NOW document: %u bytes of %u\n", (unsigned)n,
                (unsigned)EN_CFG_MAX_TOTAL);
    CHECK_EQ(n, strlen(buf));
    // Too small a buffer: 0, never a cut document.
    CHECK_EQ((int)encodeConfigTo(e, buf, 100, 0), 0);

    // WiFi: every string at its maximum, with secrets — the ingest reply.
    // §3 budgets 2 KB for the node's parse buffer.
    NodeConfig w = wifiFull();
    memset(w.net.ssid, 's', SSID_CAP - 1);
    memset(w.net.pass, 'p', PASS_CAP - 1);
    memset(w.net.host, 'h', HOST_CAP - 1);
    memset(w.net.token, 't', TOKEN_CAP - 1);
    memset(w.net.basic_user, 'u', USER_CAP - 1);
    memset(w.net.basic_pass, 'b', USER_CAP - 1);
    memset(w.net.next.ssid, 'n', SSID_CAP - 1);
    memset(w.net.next.pass, 'q', PASS_CAP - 1);
    char wb[2048];
    const size_t wn = encodeConfigTo(w, wb, sizeof(wb), NCJ_SECRETS);
    CHECK(wn > 0);
    std::printf("       widest WiFi document: %u bytes of 2048\n", (unsigned)wn);
}

// ---------------------------------------------------------------------------
// The old flat /config.json
// ---------------------------------------------------------------------------

static const char* LEGACY =
    "{\"ssid\":\"home\",\"pass\":\"hunter2hunter2\",\"host\":\"10.0.0.5\",\"port\":8080,"
    "\"token\":\"abc\",\"nodeId\":\"attic\",\"basicUser\":\"u\",\"basicPass\":\"p\","
    "\"intervalMs\":61499,\"altitudeM\":250.5,\"board\":1,\"i2cSda\":2,\"i2cScl\":14,"
    "\"oneWirePin\":13,\"pulsePin\":12,\"sdsRx\":5,\"sdsTx\":4}";

static void test_legacy_detection() {
    JsonDocument d;
    deserializeJson(d, LEGACY);
    CHECK(isLegacyWifiDoc(d.as<JsonVariantConst>()));

    JsonDocument n;
    deserializeJson(n, enc(wifiFull(), NCJ_SECRETS));
    CHECK(!isLegacyWifiDoc(n.as<JsonVariantConst>()));

    JsonDocument e;
    deserializeJson(e, "{}");
    CHECK(!isLegacyWifiDoc(e.as<JsonVariantConst>()));
    JsonDocument a;
    deserializeJson(a, "[]");
    CHECK(!isLegacyWifiDoc(a.as<JsonVariantConst>()));
}

static void test_legacy_migration() {
    // The firmware seeds defaults and its compiled sensor list first — the
    // old file never said which sensors a node has.
    NodeConfig c = configDefaults(Transport::Wifi, Hw::Esp8266);
    addSensor(c, sensorDefaults(SensorType::Ds18b20, Hw::Esp8266));
    addSensor(c, sensorDefaults(SensorType::Pulse, Hw::Esp8266));
    addSensor(c, sensorDefaults(SensorType::Sds011, Hw::Esp8266));

    JsonDocument d;
    deserializeJson(d, LEGACY);
    CHECK(migrateLegacyWifi(d.as<JsonVariantConst>(), c));

    CHECK_STREQ(c.net.ssid, "home");
    CHECK_STREQ(c.net.pass, "hunter2hunter2");
    CHECK_STREQ(c.net.host, "10.0.0.5");
    CHECK_EQ((int)c.net.port, 8080);
    CHECK_STREQ(c.net.token, "abc");
    CHECK_STREQ(c.name, "attic");
    CHECK_STREQ(c.net.basic_user, "u");
    CHECK_STREQ(c.net.basic_pass, "p");
    CHECK_EQ((int)c.interval_s, 61);            // 61.499 s, rounded
    CHECK(c.altitude_m == 250.5f);
    CHECK_EQ((int)c.board, 1);
    CHECK_EQ((int)c.i2c.sda, 2);
    CHECK_EQ((int)c.i2c.scl, 14);
    CHECK_EQ((int)c.sensors[0].pin, 13);        // oneWirePin
    CHECK_EQ((int)c.sensors[1].pin, 12);        // pulsePin
    CHECK_EQ((int)c.sensors[2].rx, 5);
    CHECK_EQ((int)c.sensors[2].tx, 4);
    CHECK_EQ((int)c.sensor_count, 3);           // the list itself is untouched
    CHECK_EQ((int)c.rev, 0);
    CHECK(c.transport == Transport::Wifi);
    ACCEPT(c);

    // Intervals under the new minimum are raised to it, not refused later.
    NodeConfig f = configDefaults(Transport::Wifi, Hw::Esp8266);
    JsonDocument s;
    deserializeJson(s, "{\"ssid\":\"x\",\"intervalMs\":5000}");
    migrateLegacyWifi(s.as<JsonVariantConst>(), f);
    CHECK_EQ((int)f.interval_s, 10);
    // Rounds half up, and the top of the accepted range (INT32_MAX ms) clamps
    // to the maximum. That used to be `(x + 500) / 1000`, which on the
    // ESP8266's 32-bit long overflows (UB) before it divides; a 64-bit host
    // long never shows it, so the values are pinned here instead.
    const char* const ms[]  = { "61500", "999", "1499", "2147483647", "2147483147" };
    const int         sec[] = { 62, 10, 10, 65535, 65535 };
    for (size_t i = 0; i < sizeof(sec) / sizeof(sec[0]); i++) {
        char js[64] = "{\"intervalMs\":";
        strAppend(js, sizeof(js), ms[i]);
        strAppend(js, sizeof(js), "}");
        JsonDocument m;
        deserializeJson(m, js);
        NodeConfig h = configDefaults(Transport::Wifi, Hw::Esp8266);
        migrateLegacyWifi(m.as<JsonVariantConst>(), h);
        CHECK_EQ((int)h.interval_s, sec[i]);
    }
    // Values that do not fit keep the default; the node is booting and
    // nobody is there to read an error.
    JsonDocument b;
    deserializeJson(b, "{\"ssid\":\"x\",\"port\":0,\"i2cSda\":300,\"nodeId\":5}");
    NodeConfig g = configDefaults(Transport::Wifi, Hw::Esp8266);
    migrateLegacyWifi(b.as<JsonVariantConst>(), g);
    CHECK_EQ((int)g.net.port, 80);
    CHECK_EQ((int)g.i2c.sda, 4);
    CHECK_STREQ(g.name, "node");
}

// ---------------------------------------------------------------------------
// caps
// ---------------------------------------------------------------------------

static void test_caps_esp8266() {
    JsonDocument d;
    encodeCaps(Transport::Wifi, Hw::Esp8266, d.to<JsonObject>());
    CHECK_STREQ(d["transport"].as<const char*>(), "wifi");
    CHECK_STREQ(d["hw"].as<const char*>(), "esp8266");
    CHECK_EQ((int)d["sensor_types"].size(), 6);
    CHECK_STREQ(d["sensor_types"][0].as<const char*>(), "bmx280");
    CHECK_STREQ(d["sensor_types"][5].as<const char*>(), "pulse");
    CHECK_EQ((int)d["sleep_unsafe"].size(), 2);
    CHECK_EQ(d["metric_count"]["bme688"].as<int>(), 5);
    CHECK_EQ(d["metric_count"]["ds18b20"].as<int>(), 1);
    CHECK_EQ(d["max_sensors"].as<int>(), 8);
    CHECK_EQ(d["max_metrics"].as<int>(), 8);
    CHECK_EQ(d["max_gpio"].as<int>(), 16);

    JsonArrayConst forb = d["forbidden_pins"].as<JsonArrayConst>();
    CHECK_EQ((int)forb.size(), 6);
    for (size_t i = 0; i < 6; i++) CHECK_EQ(forb[i].as<int>(), (int)(6 + i));

    JsonObjectConst warn = d["warn_pins"].as<JsonObjectConst>();
    CHECK_EQ((int)warn.size(), 6);
    CHECK(strstr(warn["0"].as<const char*>(), "boot strap") != nullptr);
    CHECK(!warn["16"].isNull());
    CHECK(warn["4"].isNull());

    JsonArrayConst boards = d["boards"].as<JsonArrayConst>();
    CHECK_EQ((int)boards.size(), 3);
    CHECK_EQ(boards[0]["id"].as<int>(), 0);
    CHECK_STREQ(boards[0]["name"].as<const char*>(), "NodeMCU V2/V3");
    CHECK_EQ(boards[0]["pins"]["D6"].as<int>(), 12);
    CHECK_EQ(boards[0]["pins"]["D0"].as<int>(), 16);
    CHECK_STREQ(boards[0]["left"][0].as<const char*>(), "A0");
    CHECK_EQ((int)boards[0]["right"].size(), 15);
    CHECK_STREQ(boards[1]["right"][7].as<const char*>(), "5V");
    CHECK_EQ((int)boards[2]["pins"].size(), 0);
}

static void test_caps_esp32c3() {
    JsonDocument d;
    encodeCaps(Transport::EspNow, Hw::Esp32c3, d.to<JsonObject>());
    CHECK_STREQ(d["transport"].as<const char*>(), "espnow");
    CHECK_EQ(d["max_gpio"].as<int>(), 21);
    JsonArrayConst forb = d["forbidden_pins"].as<JsonArrayConst>();
    CHECK_EQ((int)forb.size(), 6);
    CHECK_EQ(forb[0].as<int>(), 12);
    CHECK_EQ(forb[5].as<int>(), 17);
    JsonObjectConst warn = d["warn_pins"].as<JsonObjectConst>();
    const char* keys[] = { "2", "8", "9", "18", "19", "20", "21" };
    CHECK_EQ((int)warn.size(), 7);
    for (size_t i = 0; i < 7; i++) CHECK(!warn[keys[i]].isNull());
    JsonArrayConst boards = d["boards"].as<JsonArrayConst>();
    CHECK_STREQ(boards[0]["name"].as<const char*>(), "Seeed XIAO ESP32-C3");
    CHECK_EQ(boards[0]["pins"]["D6"].as<int>(), 21);
    CHECK_EQ(boards[1]["pins"]["21"].as<int>(), 21);
    CHECK(boards[2]["left"].isNull());   // "other": nothing to draw
}

// ArduinoJson 7 stores a string BY POINTER only when it is a string literal
// (StringAdapter<const char (&)[N]>, a RamString marked static). A
// `const char*` or a char array is adapted as a non-static RamString and
// copied into the document — inline when it is tiny, into the pool when not.
// encodeCaps() and the collector's handover lists pass a stack buffer as
// `(const char*)key`; this pins that the document owns those bytes, so the
// buffer may be reused or die at once.
static void test_const_char_ptr_keys_are_copied() {
    JsonDocument d;
    JsonObject o = d.to<JsonObject>();
    char buf[24];
    for (unsigned i = 0; i < 3; i++) {
        copyStr(buf, sizeof(buf), i == 1 ? "k" : "a-long-key-");   // tiny, and not
        strAppendUint(buf, sizeof(buf), i);
        o[(const char*)buf] = (int)i;
    }
    copyStr(buf, sizeof(buf), "value-that-is-not-tiny");
    o["v"] = (const char*)buf;
    memset(buf, 'X', sizeof(buf) - 1);                    // clobber the buffer
    buf[sizeof(buf) - 1] = '\0';
    char out[128];
    serializeJson(d, out, sizeof(out));
    CHECK_STREQ(out, "{\"a-long-key-0\":0,\"k1\":1,\"a-long-key-2\":2,"
                     "\"v\":\"value-that-is-not-tiny\"}");

    // And the caps' warn_pins keys, read after encodeCaps() has returned:
    // every C3 note under its own GPIO number, in table order.
    JsonDocument c;
    encodeCaps(Transport::EspNow, Hw::Esp32c3, c.to<JsonObject>());
    std::string keys;
    for (JsonPairConst kv : c["warn_pins"].as<JsonObjectConst>()) {
        keys += kv.key().c_str();
        keys += ',';
    }
    CHECK_STREQ(keys.c_str(), "2,8,9,18,19,20,21,");
}

int main() {
    RUN(test_bases_are_valid);
    RUN(test_name_rules);
    RUN(test_interval_rules);
    RUN(test_altitude_and_board_rules);
    RUN(test_sensor_count_and_types);
    RUN(test_i2c_addresses);
    RUN(test_ds18b20_fields);
    RUN(test_pulse_fields);
    RUN(test_sleep_unsafe_sensors);
    RUN(test_one_entry_per_type);
    RUN(test_bmx280_and_bme688_together);
    RUN(test_metric_budget);
    RUN(test_metric_names_unique);
    RUN(test_forbidden_and_missing_pins);
    RUN(test_pins_used_twice);
    RUN(test_interrupt_pins);
    RUN(test_battery_pin_must_be_adc);
    RUN(test_pin_warnings);
    RUN(test_battery_and_link_ranges);
    RUN(test_net_rules);
    RUN(test_lmk_rule);
    RUN(test_validation_json_shape);
    RUN(test_catalogue_ids);
    RUN(test_counts_per_type);
    RUN(test_list_metrics_order_and_names);
    RUN(test_round_trip);
    RUN(test_sections_follow_transport);
    RUN(test_secrets_are_blanked);
    RUN(test_secret_decode_rules);
    RUN(test_partial_decode);
    RUN(test_sensor_list_decode);
    RUN(test_decode_errors);
    RUN(test_lmk_decode);
    RUN(test_documents_fit_their_transports);
    RUN(test_legacy_detection);
    RUN(test_legacy_migration);
    RUN(test_caps_esp8266);
    RUN(test_caps_esp32c3);
    RUN(test_const_char_ptr_keys_are_copied);
    std::printf("       %d rejections provoked, every reason whole\n", g_reasonsSeen);
    return SUMMARY();
}
