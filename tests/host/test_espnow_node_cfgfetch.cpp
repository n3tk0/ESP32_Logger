// The ESP-NOW node's side of the config exchange (docs/NODE_CONFIG.md §5):
// the per-wake CFG fetch state machine (node_espnow/src/CfgFetch.h), what the
// node does with a finished document (node_espnow/src/CfgApply.h), the report
// it sends the other way, the backoff that keeps a broken collector from
// costing a battery, and the rescan decisions (node_espnow/src/Rescan.h).
//
// WHY THIS FILE EXISTS
// --------------------
// All of it runs on a device that is awake for a third of a second, where a
// loop that does not terminate is a flat cell and a config applied wrongly is
// a node nobody can reach any more. None of that is observable on a bench in
// the time anyone watches one. So the collector is simulated here with the
// same frame builders the real one uses, the clock is fake, and every way the
// air can misbehave — lost replies, a slow collector, a collector that never
// answers, a config that changes mid-transfer — is played out to the end.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "node_espnow/src/CfgApply.h"
#include "node_espnow/src/CfgFetch.h"
#include "node_espnow/src/Rescan.h"
#include "check.h"

using namespace nodecfg;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// What an ESP-NOW node runs out of the box: node_config.h's defaults.
static NodeConfig nodeBase() {
    NodeConfig c = configDefaults(Transport::EspNow, Hw::Esp32c3);
    copyStr(c.name, sizeof(c.name), "node-0A0B");
    copyStr(c.fw, sizeof(c.fw), "2026.09.1");
    SensorCfg b = sensorDefaults(SensorType::Bmx280, Hw::Esp32c3);
    addSensor(c, b);
    c.rev = 3;
    copyStr(c.lmk, sizeof(c.lmk), "0123456789abcdef");
    return c;
}

/// A desired config from the collector: a different interval, two more
/// sensors, a handover target — everything a real edit touches.
static NodeConfig desired() {
    NodeConfig c = nodeBase();
    c.interval_s = 120;
    c.altitude_m = 312.5f;
    SensorCfg ds = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    ds.count = 2;
    copyStr(ds.metric, sizeof(ds.metric), "pond");
    addSensor(c, ds);
    addSensor(c, sensorDefaults(SensorType::Bh1750, Hw::Esp32c3));
    c.link.ack_window_ms = 40;
    copyStr(c.link.next_ssid, sizeof(c.link.next_ssid), "home-new");
    c.batt.trim = 1.02f;
    c.lmk[0] = '\0';        // the collector never has it
    copyStr(c.fw, sizeof(c.fw), "");
    return c;
}

/// The collector, as far as CFG_GET is concerned.
struct SimCollector {
    uint8_t  nodeId = 7;
    uint16_t rev    = 0;
    uint16_t total  = 0;
    char     doc[EN_CFG_MAX_TOTAL + 1];
    uint32_t latencyMs = 3;      ///< reply time when it answers
    int      dropEvery = 0;      ///< drop every Nth reply (0 = never)
    bool     silent    = false;
    int      served    = 0;

    void load(const NodeConfig& c, uint16_t r) {
        rev = r;
        NodeConfig tmp = c;
        tmp.rev = r;
        total = (uint16_t)encodeConfigTo(tmp, doc, sizeof(doc), 0);
    }
    /// Answer one request. False = nothing came back.
    bool answer(const CfgGetMsg& q, CfgChunkMsg& out) {
        if (silent || q.nodeId != nodeId) return false;
        served++;
        if (dropEvery && served % dropEvery == 0) return false;
        if (q.offset >= total) return espnowFillCfgChunk(out, EN_MSG_CFG, nodeId, rev, doc, 0, 0) > 0;
        const int len = espnowFillCfgChunk(out, EN_MSG_CFG, nodeId, rev, doc, total, q.offset);
        // Everything the node receives has passed the collector's validator
        // first on the air — check it would.
        uint8_t type = 0;
        CHECK(espnowValidate((const uint8_t*)&out, len, type));
        return len > 0;
    }
};

struct RunResult {
    encfg::Step step;
    bool        gaveUp;
    int         requests;
    uint32_t    spentMs;
};

/// One wake's fetch, as main.cpp's fetchConfig() drives it, on a fake clock.
static encfg::Fetch g_f;
static RunResult runFetch(SimCollector& col, uint16_t haveRev, uint16_t rejectedRev,
                          uint32_t budgetMs = 400, uint32_t windowMs = 50) {
    RunResult r{encfg::Step::Request, false, 0, 0};
    uint32_t now = 100000;
    encfg::begin(g_f, col.nodeId, haveRev, rejectedRev, now, budgetMs, 2);
    CfgGetMsg q;
    CfgChunkMsg m;
    for (int guard = 0; guard < 1000; guard++) {
        if (!encfg::wantRequest(g_f, now, q)) { r.gaveUp = true; break; }
        CHECK_EQ(q.type, EN_MSG_CFG_GET);
        CHECK_EQ(q.haveRev, haveRev);
        uint32_t w = encfg::remainingMs(g_f, now);
        if (w > windowMs) w = windowMs;
        r.requests++;
        const bool got = col.answer(q, m) && col.latencyMs <= w;
        now += got ? col.latencyMs : w;
        r.step = encfg::onReply(g_f, got ? &m : nullptr);
        if (r.step != encfg::Step::Request) break;
    }
    r.spentMs = now - 100000;
    return r;
}

// ---------------------------------------------------------------------------
// Fetch
// ---------------------------------------------------------------------------

static void test_a_fetch_pulls_every_slice_in_order_and_applies() {
    SimCollector col;
    col.load(desired(), 4);
    CHECK(col.total > EN_CFG_CHUNK_MAX);               // a multi-slice document
    RunResult r = runFetch(col, 3, 0);
    CHECK(r.step == encfg::Step::Complete);
    CHECK(!r.gaveUp);
    CHECK_EQ(r.requests, (col.total + EN_CFG_CHUNK_MAX - 1) / EN_CFG_CHUNK_MAX);
    CHECK_EQ(g_f.a.total, col.total);
    CHECK_EQ(g_f.a.rev, 4);
    CHECK(memcmp(g_f.a.doc, col.doc, col.total) == 0);

    const NodeConfig running = nodeBase();
    static NodeConfig next;
    Issue why;
    CHECK(encfg::acceptFromCollector(running, g_f.a.doc, g_f.a.total, g_f.a.rev, next, why));
    CHECK_EQ(next.rev, 4);
    CHECK(!next.local);
    CHECK_EQ(next.interval_s, 120);
    CHECK_EQ(next.sensor_count, 3);
    CHECK(next.sensors[1].type == SensorType::Ds18b20);
    CHECK_STREQ(next.sensors[1].metric, "pond");
    CHECK_EQ(next.link.ack_window_ms, 40);
    CHECK_STREQ(next.link.next_ssid, "home-new");
    CHECK(fabsf(next.altitude_m - 312.5f) < 0.001f);
    // Never taken from the radio: the node's own key and firmware string.
    CHECK_STREQ(next.lmk, "0123456789abcdef");
    CHECK_STREQ(next.fw, "2026.09.1");
    CHECK(next.transport == Transport::EspNow);
    CHECK(next.hw == Hw::Esp32c3);
}

static void test_lost_replies_are_asked_for_again() {
    SimCollector col;
    col.load(desired(), 4);
    col.dropEvery = 2;                                 // every other reply lost
    RunResult r = runFetch(col, 3, 0);
    CHECK(r.step == encfg::Step::Complete);
    CHECK(memcmp(g_f.a.doc, col.doc, col.total) == 0);
    CHECK(r.spentMs <= 400);
}

static void test_a_silent_collector_costs_a_bounded_time() {
    SimCollector col;
    col.load(desired(), 4);
    col.silent = true;
    RunResult r = runFetch(col, 3, 0);
    CHECK(r.gaveUp);
    CHECK_EQ(r.requests, 2);                           // maxMisses, not the budget
    CHECK_EQ(r.spentMs, 100u);                         // two 50 ms windows
}

static void test_a_slow_collector_hits_the_budget_and_the_next_wake_restarts() {
    SimCollector col;
    col.load(desired(), 4);
    col.latencyMs = 180;                               // answers, but slowly
    RunResult r = runFetch(col, 3, 0, 400, 200);
    CHECK(r.gaveUp);
    CHECK(r.spentMs <= 400);                           // THE ceiling
    CHECK(g_f.offset > 0);                             // got somewhere, not everywhere

    col.latencyMs = 3;
    r = runFetch(col, 3, 0);                           // next wake: from offset 0
    CHECK(r.step == encfg::Step::Complete);
    CHECK(memcmp(g_f.a.doc, col.doc, col.total) == 0);
}

static void test_a_config_that_changes_mid_transfer_restarts_at_zero() {
    SimCollector col;
    col.load(desired(), 4);
    uint32_t now = 0;
    encfg::begin(g_f, 7, 3, 0, now, 400, 2);
    CfgGetMsg q;
    CfgChunkMsg m;
    CHECK(encfg::wantRequest(g_f, now, q));
    CHECK(col.answer(q, m));
    CHECK(encfg::onReply(g_f, &m) == encfg::Step::Request);
    CHECK_EQ(g_f.offset, EN_CFG_CHUNK_MAX);

    // The user saves again on the collector: rev 5, a different document.
    NodeConfig d2 = desired();
    d2.interval_s = 300;
    col.load(d2, 5);
    CHECK(encfg::wantRequest(g_f, now, q));
    CHECK(col.answer(q, m));                           // slice 1 of rev 5
    CHECK(encfg::onReply(g_f, &m) == encfg::Step::Request);
    CHECK_EQ(g_f.offset, 0);                           // start rev 5 over
    for (int i = 0; i < 10; i++) {
        CHECK(encfg::wantRequest(g_f, now, q));
        CHECK(col.answer(q, m));
        const encfg::Step s = encfg::onReply(g_f, &m);
        if (s == encfg::Step::Complete) break;
        CHECK(s == encfg::Step::Request);
    }
    CHECK_EQ(g_f.a.rev, 5);
    CHECK_EQ(g_f.a.total, col.total);
    CHECK(memcmp(g_f.a.doc, col.doc, col.total) == 0);
}

static void test_what_we_already_run_or_already_refused_is_not_downloaded() {
    SimCollector col;
    col.load(desired(), 4);
    RunResult r = runFetch(col, 4, 0);                 // our CFG_ACK was lost
    CHECK(r.step == encfg::Step::UpToDate);
    CHECK_EQ(g_f.rev, 4);
    CHECK_EQ(r.requests, 1);

    r = runFetch(col, 3, 4);                           // refused rev 4 already
    CHECK(r.step == encfg::Step::KnownRejected);
    CHECK_EQ(g_f.rev, 4);
    CHECK_EQ(r.requests, 1);

    // total == 0 is "nothing for you".
    CfgChunkMsg z;
    CHECK(espnowFillCfgChunk(z, EN_MSG_CFG, 7, 9, nullptr, 0, 0) == EN_CFG_CHUNK_HDR);
    encfg::begin(g_f, 7, 3, 0, 0, 400, 2);
    CHECK(encfg::onReply(g_f, &z) == encfg::Step::UpToDate);
    CHECK_EQ(g_f.rev, 9);
}

static void test_frames_for_someone_else_do_not_count() {
    SimCollector col;
    col.load(desired(), 4);
    CfgChunkMsg m;
    CHECK(espnowFillCfgChunk(m, EN_MSG_CFG, 8, 4, col.doc, col.total, 0) > 0);  // node 8's
    encfg::begin(g_f, 7, 3, 0, 0, 400, 2);
    CHECK(encfg::onReply(g_f, &m) == encfg::Step::Request);
    CHECK_EQ(g_f.misses, 1);
    CHECK_EQ(g_f.a.have, 0);
    // A CFG_REPORT (the other direction) is not a CFG either.
    CHECK(espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, 7, 4, col.doc, col.total, 0) > 0);
    CHECK(encfg::onReply(g_f, &m) == encfg::Step::Request);
    CHECK_EQ(g_f.misses, 2);
    CfgGetMsg q;
    CHECK(!encfg::wantRequest(g_f, 0, q));             // two misses: done
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

static void test_a_config_the_validator_refuses_leaves_the_node_alone() {
    const NodeConfig running = nodeBase();
    static NodeConfig next;
    Issue why;

    // An SDS011 on a node that deep sleeps: §1.2.
    NodeConfig bad = nodeBase();
    addSensor(bad, sensorDefaults(SensorType::Sds011, Hw::Esp32c3));
    char doc[EN_CFG_MAX_TOTAL + 1];
    uint16_t n = (uint16_t)encodeConfigTo(bad, doc, sizeof(doc), 0);
    CHECK(!encfg::acceptFromCollector(running, doc, n, 5, next, why));
    CHECK_STREQ(why.field, "sensors[1].type");
    CHECK(why.reason[0] != '\0');

    // …and the same thing with sleep off is fine: mains mode.
    bad.sleep = false;
    n = (uint16_t)encodeConfigTo(bad, doc, sizeof(doc), 0);
    CHECK(encfg::acceptFromCollector(running, doc, n, 5, next, why));
    CHECK(!next.sleep);

    // A shape error names the field too.
    const char* shape = "{\"sensors\":[{\"type\":\"bmx280\"},{\"type\":\"ds18b20\",\"pin\":262}]}";
    CHECK(!encfg::acceptFromCollector(running, shape, (uint16_t)strlen(shape), 5, next, why));
    CHECK_STREQ(why.field, "sensors[1].pin");

    // Not JSON at all.
    CHECK(!encfg::acceptFromCollector(running, "{\"name\":", 8, 5, next, why));
    CHECK(why.reason[0] != '\0');
    CHECK(strlen(why.field) < EN_CFG_FIELD_LEN);
    CHECK(strlen(why.reason) < EN_CFG_REASON_LEN);
}

static void test_a_partial_document_is_an_edit_and_the_body_cannot_forge_identity() {
    const NodeConfig running = nodeBase();
    static NodeConfig next;
    Issue why;
    const char* doc = "{\"rev\":99,\"local\":true,\"transport\":\"wifi\",\"hw\":\"esp8266\","
                      "\"fw\":\"evil\",\"lmk\":\"AAAAAAAAAAAAAAAA\",\"interval_s\":30}";
    CHECK(encfg::acceptFromCollector(running, doc, (uint16_t)strlen(doc), 6, next, why));
    CHECK_EQ(next.rev, 6);                              // the frame's, not the body's
    CHECK(!next.local);
    CHECK_EQ(next.interval_s, 30);
    CHECK_EQ(next.sensor_count, running.sensor_count); // untouched
    CHECK(next.transport == Transport::EspNow);
    CHECK(next.hw == Hw::Esp32c3);
    CHECK_STREQ(next.fw, "2026.09.1");
    CHECK_STREQ(next.lmk, "0123456789abcdef");         // principle 6
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

/// The fattest valid ESP-NOW configs there are must still fit the 1 KB the
/// radio carries — or the node could hold a config it can never report.
static void test_the_largest_valid_configs_fit_one_kilobyte() {
    // Eight ds18b20 buses with ten-character names: the most entries.
    NodeConfig a = configDefaults(Transport::EspNow, Hw::Esp32c3);
    copyStr(a.name, sizeof(a.name), "abcdefghijklmnop");
    copyStr(a.fw, sizeof(a.fw), "2026.09.1-dirty-abcdefg");
    copyStr(a.link.next_ssid, sizeof(a.link.next_ssid), "0123456789abcdef0123456789abcdef");
    a.rev = 65535;
    a.local = true;
    a.altitude_m = -432.25f;
    a.link.rescan_min_s = 2000000000u;
    a.batt.divider = 3.14159f;
    a.batt.trim = 0.987654f;
    const uint8_t pins[8] = {0, 1, 3, 4, 5, 8, 10, 20};
    for (uint8_t i = 0; i < 8; i++) {
        SensorCfg s = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
        s.pin = pins[i];
        copyStr(s.metric, sizeof(s.metric), "abcdefghi");
        s.metric[9] = (char)('a' + i);
        s.metric[10] = '\0';
        addSensor(a, s);
    }
    a.sleep = true;
    Validation v;
    CHECK(validate(a, v));
    if (!v.ok) printf("  (%s: %s)\n", v.error.field, v.error.reason);
    char doc[2048];
    size_t n = encodeConfigTo(a, doc, sizeof(doc), 0);
    printf("  eight ds18b20 buses: %u bytes\n", (unsigned)n);
    CHECK(n > 0 && n <= EN_CFG_MAX_TOTAL);

    // Every kind of sensor, mains powered, with awkward floats.
    NodeConfig b = a;
    b.sensor_count = 0;
    b.sleep = false;
    addSensor(b, sensorDefaults(SensorType::Bh1750, Hw::Esp32c3));
    SensorCfg sds = sensorDefaults(SensorType::Sds011, Hw::Esp32c3);
    addSensor(b, sds);
    SensorCfg pu = sensorDefaults(SensorType::Pulse, Hw::Esp32c3);
    pulseModeDefaults(pu, PulseMode::Flow);
    pu.per_pulse = 0.00123457f;
    pu.debounce_us = 999999;
    addSensor(b, pu);
    const uint8_t dsPins[3] = {0, 3, 20};
    for (uint8_t i = 0; i < 3; i++) {
        SensorCfg s = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
        s.pin = dsPins[i];
        copyStr(s.metric, sizeof(s.metric), "zyxwvutsr");
        s.metric[9] = (char)('a' + i);
        s.metric[10] = '\0';
        addSensor(b, s);
    }
    CHECK(validate(b, v));
    if (!v.ok) printf("  (%s: %s)\n", v.error.field, v.error.reason);
    n = encodeConfigTo(b, doc, sizeof(doc), 0);
    printf("  every sensor type: %u bytes\n", (unsigned)n);
    CHECK(n > 0 && n <= EN_CFG_MAX_TOTAL);
}

static void test_a_report_reassembles_into_the_same_config_and_never_carries_the_key() {
    NodeConfig c = desired();
    copyStr(c.lmk, sizeof(c.lmk), "0123456789abcdef");
    copyStr(c.fw, sizeof(c.fw), "2026.09.1");
    c.rev = 4;
    c.local = true;
    char doc[EN_CFG_MAX_TOTAL + 1];
    const uint16_t n = (uint16_t)encodeConfigTo(c, doc, sizeof(doc), 0);   // what reportConfig() sends
    CHECK(n > 0);
    CHECK(strstr(doc, "lmk") == nullptr);
    CHECK(strstr(doc, "0123456789abcdef") == nullptr);
    CHECK(strstr(doc, "\"net\"") == nullptr);

    // Slice it exactly as reportConfig() does, and reassemble as the collector.
    static EnCfgAssembler a;
    espnowCfgReset(a);
    CfgChunkMsg m;
    int frames = 0;
    EnCfgFeed last = EN_CFG_FEED_IGNORED;
    for (uint16_t off = 0; off < n; off = (uint16_t)(off + m.len)) {
        const int flen = espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, 7, c.rev, doc, n, off);
        CHECK(flen > 0 && flen <= ESPNOW_MAX_FRAME);
        uint8_t type = 0;
        CHECK(espnowValidate((const uint8_t*)&m, flen, type));
        CHECK_EQ(type, EN_MSG_CFG_REPORT);
        last = espnowCfgFeed(a, m);
        frames++;
    }
    CHECK_EQ(last, EN_CFG_FEED_DONE);
    CHECK_EQ(frames, (n + EN_CFG_CHUNK_MAX - 1) / EN_CFG_CHUNK_MAX);

    JsonDocument jd;
    CHECK(deserializeJson(jd, a.doc, a.total) == DeserializationError::Ok);
    NodeConfig got = configDefaults(Transport::EspNow, Hw::Esp32c3);
    Issue why;
    CHECK(decodeConfig(jd.as<JsonVariantConst>(), got, NCJ_DEC_REV | NCJ_DEC_IDENTITY, &why));
    CHECK_EQ(got.rev, 4);
    CHECK(got.local);
    CHECK_STREQ(got.fw, "2026.09.1");
    CHECK_EQ(got.interval_s, 120);
    CHECK_EQ(got.sensor_count, 3);
    CHECK_STREQ(got.sensors[1].metric, "pond");
    CHECK_STREQ(got.link.next_ssid, "home-new");
    CHECK_STREQ(got.lmk, "");
}

// ---------------------------------------------------------------------------
// Backoff
// ---------------------------------------------------------------------------

static void test_backoff_doubles_to_an_hour_and_resets_on_success() {
    encfg::Backoff b{0, 0};
    CHECK(encfg::due(b));
    int expected[] = {1, 3, 7, 15, 31, 63, 63, 63};
    for (int k = 0; k < 8; k++) {
        encfg::failed(b);
        int skipped = 0;
        while (!encfg::due(b)) skipped++;
        CHECK_EQ(skipped, expected[k]);
    }
    encfg::succeeded(b);
    CHECK(encfg::due(b));
    CHECK(encfg::due(b));
}

// ---------------------------------------------------------------------------
// Rescan
// ---------------------------------------------------------------------------

static void test_the_rescan_gate_is_counted_in_wakes() {
    using enrescan::due;
    // Three failures and never scanned: now.
    CHECK(due(3, 0xFFFF, 3, 3600, 60));
    CHECK(!due(2, 0xFFFF, 3, 3600, 60));
    // Scanned 59 wakes ago at 60 s: not yet; 60: yes.
    CHECK(!due(9, 59, 3, 3600, 60));
    CHECK(due(9, 60, 3, 3600, 60));
    // A longer interval means fewer wakes to the hour.
    CHECK(due(9, 12, 3, 3600, 300));
    // rescan_min_s 0 = no ceiling; rescan_fails 0 is treated as 1; interval 0
    // as 60 rather than a division by zero.
    CHECK(due(1, 0, 1, 0, 60));
    CHECK(due(1, 0, 0, 0, 60));
    CHECK(due(1, 60, 1, 3600, 0));
}

static void test_what_a_scan_decides() {
    using enrescan::Action;
    using enrescan::decide;
    // The router re-picked its channel.
    CHECK(decide(11, 0, 6) == Action::MoveChannel);
    CHECK(decide(11, 1, 6) == Action::MoveChannel);    // stored network still wins
    // Handover while we slept: the old network is gone, next is up.
    CHECK(decide(0, 1, 6) == Action::AdoptNext);
    CHECK(decide(0, 6, 6) == Action::AdoptNext);       // same channel, new name
    // The old network stayed up, the collector moved to next elsewhere.
    CHECK(decide(6, 1, 6) == Action::AdoptNext);
    // Nothing moved and nobody answers, or nothing is on the air: sweep.
    CHECK(decide(6, 0, 6) == Action::Sweep);
    CHECK(decide(6, 6, 6) == Action::Sweep);
    CHECK(decide(0, 0, 6) == Action::Sweep);
    // A node that never stored a channel follows whatever it finds.
    CHECK(decide(3, 0, 0) == Action::MoveChannel);
}

int main() {
    RUN(test_a_fetch_pulls_every_slice_in_order_and_applies);
    RUN(test_lost_replies_are_asked_for_again);
    RUN(test_a_silent_collector_costs_a_bounded_time);
    RUN(test_a_slow_collector_hits_the_budget_and_the_next_wake_restarts);
    RUN(test_a_config_that_changes_mid_transfer_restarts_at_zero);
    RUN(test_what_we_already_run_or_already_refused_is_not_downloaded);
    RUN(test_frames_for_someone_else_do_not_count);
    RUN(test_a_config_the_validator_refuses_leaves_the_node_alone);
    RUN(test_a_partial_document_is_an_edit_and_the_body_cannot_forge_identity);
    RUN(test_the_largest_valid_configs_fit_one_kilobyte);
    RUN(test_a_report_reassembles_into_the_same_config_and_never_carries_the_key);
    RUN(test_backoff_doubles_to_an_hour_and_resets_on_success);
    RUN(test_the_rescan_gate_is_counted_in_wakes);
    RUN(test_what_a_scan_decides);
    return SUMMARY();
}
