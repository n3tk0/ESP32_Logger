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
#include "src/nodes/NodeCfgRules.h"
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
// Reporting until the collector says it landed (§5 CFG_REPORT)
// ---------------------------------------------------------------------------

/// The collector's side of CFG_REPORT as src/espnow/EspNowIngest.cpp's
/// feedCfgReport() does it: ONE assembler for every node, held against
/// another node's first slice while a transfer is moving, a ready slot the
/// tick empties, and an answer (CFG, total 0) to every complete report.
/// `old`: a collector from before that — it answered only a local report.
struct SimReportCollector {
    EnCfgAssembler a;
    uint32_t lastMs = 0;
    bool     ready  = false;
    bool     old    = false;
    int      landed[256];
    char     got[256][EN_CFG_MAX_TOTAL + 1];
    uint16_t desiredRev[256];            ///< 0 = no config held for that node

    SimReportCollector() {
        espnowCfgReset(a);
        memset(landed, 0, sizeof(landed));
        memset(desiredRev, 0, sizeof(desiredRev));
    }
    /// One slice heard at `now`. True, with `out`, when it answers.
    bool hear(const CfgChunkMsg& c, uint32_t now, CfgChunkMsg& out) {
        uint8_t type = 0;
        CHECK(espnowValidate((const uint8_t*)&c, espnowCfgChunkLen(c.len), type));
        if (ready) return false;
        if (espnowCfgHeld(a, c, now - lastMs)) return false;
        const EnCfgFeed f = espnowCfgFeed(a, c);
        if (f == EN_CFG_FEED_IGNORED) return false;
        lastMs = now;
        if (f != EN_CFG_FEED_DONE) return false;
        ready = true;
        const bool local = strstr(a.doc, "\"local\":true") != nullptr;
        const uint16_t d = desiredRev[a.nodeId];
        const ncr::ReportPlan plan = ncr::planReport(d != 0, d, a.rev, local);
        if (plan.adopt) desiredRev[a.nodeId] = plan.rev;
        landed[a.nodeId]++;
        memcpy(got[a.nodeId], a.doc, a.total + 1);
        if (old && !local) return false;
        return espnowFillCfgChunk(out, EN_MSG_CFG, a.nodeId,
                                  ncr::reportAnswerRev(plan, a.rev, local), nullptr, 0, 0) > 0;
    }
    /// The loop() tick: stores the report and frees the assembler.
    void tick() {
        if (ready) espnowCfgReset(a);
        ready = false;
    }
};

/// A node's report, as main.cpp's reportConfig() sends it: every slice, the
/// last one waiting for the answer, the report kept owed until it comes. The
/// decisions are CfgFetch.h's own (choose, reportAnswered, Backoff).
struct SimNode {
    uint8_t        id;
    NodeConfig     cfg;
    bool           reportDue = true;           // a boot just happened
    encfg::Backoff report{0, 0};
    encfg::Backoff fetch{0, 0};
    int            reports = 0;                // attempts
    int            fetches = 0;
    // one attempt in flight
    char           doc[EN_CFG_MAX_TOTAL + 1];
    uint16_t       n   = 0;
    uint16_t       off = 0;
    bool           sending = false;
    bool           deaf    = false;             // the next answer is lost in the air

    SimNode(uint8_t nodeId, uint16_t rev) : id(nodeId), cfg(desired()) {
        cfg.rev = rev;
        char nm[16];
        snprintf(nm, sizeof(nm), "node-%02X", nodeId);
        copyStr(cfg.name, sizeof(cfg.name), nm);
    }
    /// After the wake's ACK: what the node does next.
    encfg::Exchange wake(bool cfgPending) {
        const encfg::Exchange x = encfg::choose(cfg.local, reportDue, cfgPending, report, fetch);
        if (x == encfg::Exchange::Report) {
            n = (uint16_t)encodeConfigTo(cfg, doc, sizeof(doc), 0);
            CHECK(n > EN_CFG_CHUNK_MAX);             // several slices: they can interleave
            off = 0;
            sending = true;
            reports++;
        }
        if (x == encfg::Exchange::Fetch) {
            fetches++;
            encfg::succeeded(fetch);
        }
        return x;
    }
    /// The next slice; `last` = the one that waits for the answer.
    void next(CfgChunkMsg& m, bool& last) {
        espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, id, cfg.rev, doc, n, off);
        off = (uint16_t)(off + m.len);
        last = off >= n;
    }
    /// The window after the last slice closed with `answer` (nullptr: none).
    void finish(const CfgChunkMsg* answer) {
        sending = false;
        if (deaf) answer = nullptr;
        deaf = false;
        if (!encfg::reportAnswered(answer, id, cfg.local)) {
            encfg::failed(report);
            return;
        }
        if (cfg.local) {
            cfg.rev   = answer->rev;
            cfg.local = false;
        }
        reportDue = false;
        encfg::succeeded(report);
    }
};

/// One wake for every node in `nodes`, all at once (a power cut is over):
/// their slices go out round-robin, 2 ms apart, as the air interleaves them.
/// `lose(k)` drops the k-th slice of the wake.
template <typename Lose>
static void wakeTogether(SimReportCollector& col, SimNode* const* nodes, int count,
                         uint32_t& now, Lose lose) {
    for (int i = 0; i < count; i++) nodes[i]->wake(false);
    int k = 0;
    for (bool any = true; any;) {
        any = false;
        for (int i = 0; i < count; i++) {
            SimNode& nd = *nodes[i];
            if (!nd.sending) continue;
            any = true;
            CfgChunkMsg m, rep;
            bool last = false;
            nd.next(m, last);
            now += 2;
            const bool heard    = !lose(k++);
            const bool answered = heard && col.hear(m, now, rep);
            if (last) nd.finish(answered ? &rep : nullptr);
            else      CHECK(!answered);           // only a last slice is answered
        }
    }
    col.tick();
    now += 60000;                                  // a minute's sleep
}

static bool never(int) { return false; }

static void test_a_lost_boot_report_is_sent_again_until_answered() {
    SimReportCollector col;
    SimNode a(7, 3);
    SimNode* nodes[] = {&a};
    uint32_t now = 1000;

    // Wake 1: the second slice is lost on the air. The radio's delivery of
    // the other slices proves nothing; no answer comes, the report stays owed.
    wakeTogether(col, nodes, 1, now, [](int k) { return k == 1; });
    CHECK(a.reportDue);
    CHECK_EQ(col.landed[7], 0);
    // Wake 2 is skipped by the backoff; wake 3 reports again and it lands.
    wakeTogether(col, nodes, 1, now, never);
    CHECK_EQ(a.reports, 1);
    wakeTogether(col, nodes, 1, now, never);
    CHECK_EQ(a.reports, 2);
    CHECK(!a.reportDue);
    CHECK_EQ(col.landed[7], 1);
    CHECK_STREQ(col.got[7], a.doc);
    // The answer to a report after a boot moved nothing on the node.
    CHECK_EQ(a.cfg.rev, 3);
    CHECK(!a.cfg.local);
    // And it is not sent again.
    for (int w = 0; w < 10; w++) wakeTogether(col, nodes, 1, now, never);
    CHECK_EQ(a.reports, 2);
    CHECK_EQ(col.landed[7], 1);

    // A node the collector has never seen, at rev 0: adopted at 1, and the
    // answer hands the node its own rev back — it stays at 0 and fetches 1.
    SimNode b(9, 0);
    SimNode* nb[] = {&b};
    wakeTogether(col, nb, 1, now, never);
    CHECK(!b.reportDue);
    CHECK_EQ(col.desiredRev[9], 1);
    CHECK_EQ(b.cfg.rev, 0);

    // The answer itself lost (the collector has the report, the node does
    // not know): the node reports again, and the repeat is a status report —
    // in step, not adopted a second time at a new rev.
    SimNode c(10, 5);
    SimNode* nc[] = {&c};
    c.deaf = true;
    wakeTogether(col, nc, 1, now, never);
    CHECK(c.reportDue);
    CHECK_EQ(col.landed[10], 1);
    CHECK_EQ(col.desiredRev[10], 5);
    wakeTogether(col, nc, 1, now, never);       // skipped by the backoff
    wakeTogether(col, nc, 1, now, never);
    CHECK(!c.reportDue);
    CHECK_EQ(col.landed[10], 2);
    CHECK_EQ(col.desiredRev[10], 5);
    CHECK_EQ(c.cfg.rev, 5);
}

static void test_nodes_booting_together_all_land_eventually() {
    SimReportCollector col;
    SimNode a(7, 3), b(8, 3), c(9, 0);
    c.cfg.local = true;                      // edited on its page before the cut
    SimNode* nodes[] = {&a, &b, &c};
    uint32_t now = 1000;

    // Wake 1: three reports on the air at once, slices interleaved. The
    // first to start holds the assembler; the others' slices are dropped,
    // not allowed to wipe it — so exactly one lands whole.
    wakeTogether(col, nodes, 3, now, never);
    CHECK_EQ(col.landed[7], 1);
    CHECK_STREQ(col.got[7], a.doc);
    CHECK(!a.reportDue);
    CHECK(b.reportDue);
    CHECK(c.cfg.local);
    CHECK_EQ(col.landed[8] + col.landed[9], 0);

    int wakes = 1;
    while ((b.reportDue || c.cfg.local || c.reportDue) && wakes < 64) {
        wakeTogether(col, nodes, 3, now, never);
        wakes++;
    }
    CHECK(!b.reportDue);
    CHECK(!c.reportDue);
    CHECK(!c.cfg.local);
    CHECK_EQ(col.landed[7], 1);
    CHECK_EQ(col.landed[8], 1);
    CHECK_EQ(col.landed[9], 1);
    CHECK_STREQ(col.got[8], b.doc);
    CHECK_EQ(c.cfg.rev, 1);                  // the local edit was adopted and said so
    CHECK_EQ(col.desiredRev[9], 1);
    CHECK(wakes <= 8);
    printf("  three nodes after a power cut: all reports landed by wake %d\n", wakes);

    // A transfer that stalls (its node gave up) is not held for ever: once
    // it is EN_CFG_HOLD_MS old, another node's report starts over it.
    SimReportCollector col2;
    SimNode d(11, 2), e(12, 2);
    SimNode* nd[] = {&d};
    SimNode* ne[] = {&e};
    uint32_t t = 5000;
    wakeTogether(col2, nd, 1, t, [](int k) { return k == 1; });   // d stalls after slice 0
    CHECK(d.reportDue);
    CHECK(col2.a.have > 0 && col2.a.have < col2.a.total);          // still half of d's
    t -= 60000;                                                    // e, EN_CFG_HOLD_MS later
    t += EN_CFG_HOLD_MS;
    wakeTogether(col2, ne, 1, t, never);
    CHECK(!e.reportDue);
    CHECK_EQ(col2.landed[12], 1);
}

static void test_a_collector_that_never_answers_costs_bounded_reports() {
    SimReportCollector col;
    col.old = true;                          // answers only a local report
    SimNode a(7, 3);
    SimNode* nodes[] = {&a};
    uint32_t now = 1000;

    // A day of one-minute wakes. The report lands every time — the old
    // collector just never says so — and the node cannot know: it keeps
    // asking, but the backoff makes that one report per 64 wakes, not 1440.
    for (int w = 0; w < 1440; w++) wakeTogether(col, nodes, 1, now, never);
    printf("  a collector that never answers: %d reports in a day of wakes\n", a.reports);
    CHECK(a.reportDue);
    CHECK(a.reports >= 2);
    CHECK(a.reports <= 6 + 1440 / 64 + 1);
    CHECK_EQ(col.landed[7], a.reports);

    // And the report it owes never holds up a pending config: on every wake
    // the report backoff skips, a flagged config is fetched.
    encfg::Backoff rb{0, 0}, fb{0, 0};
    CHECK(encfg::choose(false, true, true, rb, fb) == encfg::Exchange::Report);
    encfg::failed(rb);
    CHECK(encfg::choose(false, true, true, rb, fb) == encfg::Exchange::Fetch);
    CHECK(encfg::choose(false, true, true, rb, fb) == encfg::Exchange::Report);
    // A local edit, though, is reported before anything is fetched, and
    // nothing is fetched while it waits.
    encfg::failed(rb);
    CHECK(encfg::choose(true, true, true, rb, fb) == encfg::Exchange::None);
    CHECK_EQ(fb.skip, 0);
    CHECK(encfg::choose(false, false, true, rb, fb) == encfg::Exchange::Fetch);
    CHECK(encfg::choose(false, false, false, rb, fb) == encfg::Exchange::None);
}

static void test_what_counts_as_the_answer_to_a_report() {
    CfgChunkMsg m;
    espnowFillCfgChunk(m, EN_MSG_CFG, 7, 0, nullptr, 0, 0);
    CHECK(encfg::reportAnswered(&m, 7, false));      // after a boot: rev 0 is fine
    CHECK(!encfg::reportAnswered(&m, 7, true));      // a local edit is never adopted at 0
    espnowFillCfgChunk(m, EN_MSG_CFG, 7, 4, nullptr, 0, 0);
    CHECK(encfg::reportAnswered(&m, 7, true));
    CHECK(!encfg::reportAnswered(&m, 8, true));      // somebody else's
    CHECK(!encfg::reportAnswered(nullptr, 7, false));
    char doc[] = "{\"rev\":4}";
    espnowFillCfgChunk(m, EN_MSG_CFG, 7, 4, doc, (uint16_t)strlen(doc), 0);
    CHECK(!encfg::reportAnswered(&m, 7, false));     // a slice is not the answer
    espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, 7, 4, nullptr, 0, 0);
    CHECK(!encfg::reportAnswered(&m, 7, false));
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
    RUN(test_a_lost_boot_report_is_sent_again_until_answered);
    RUN(test_nodes_booting_together_all_land_eventually);
    RUN(test_a_collector_that_never_answers_costs_bounded_reports);
    RUN(test_what_counts_as_the_answer_to_a_report);
    RUN(test_backoff_doubles_to_an_hour_and_resets_on_success);
    RUN(test_the_rescan_gate_is_counted_in_wakes);
    RUN(test_what_a_scan_decides);
    return SUMMARY();
}
