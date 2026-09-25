// The WiFi node's config and link decisions — node/src/NodeSync.h.
//
// WHY THIS FILE EXISTS
// --------------------
// docs/NODE_CONFIG.md §3 and §4 give the node a handful of rules: which
// config in an ingest reply to take, which change restarts it, which network
// to try when its own has gone, when to look for the collector on the LAN,
// and when a config the collector sent is given up on and rolled back. Every
// one of them is a comparison or a counter, and every one of them fails
// without a symptom anyone on the bench would see:
//
//   * a refused config re-applied every cycle looks like a node that is busy;
//   * a restart missed on a sensor change leaves the old driver running, and
//     the readings keep arriving — under the old sensor's names;
//   * a network alternation that never reaches `next` is a node that simply
//     stayed behind when the collector moved, three weeks later;
//   * a rollback counter that counts answered cycles throws away a config
//     that worked.
//
// So the rules are a header with no Arduino in it, and this drives them.
#include "node/src/NodeSync.h"
#include "src/nodecfg/NodeConfigJson.h"
#include "check.h"

using namespace NodeSync;
using nodecfg::NodeConfig;

static NodeConfig base() {
    NodeConfig c = nodecfg::configDefaults(nodecfg::Transport::Wifi, nodecfg::Hw::Esp8266);
    nodecfg::copyStr(c.name, sizeof(c.name), "balcony");
    nodecfg::copyStr(c.net.ssid, sizeof(c.net.ssid), "home");
    nodecfg::copyStr(c.net.pass, sizeof(c.net.pass), "hunter22");
    nodecfg::copyStr(c.net.host, sizeof(c.net.host), "192.168.1.50");
    nodecfg::copyStr(c.net.token, sizeof(c.net.token), "tok");
    nodecfg::addSensor(c, nodecfg::sensorDefaults(nodecfg::SensorType::Bmx280, c.hw));
    nodecfg::addSensor(c, nodecfg::sensorDefaults(nodecfg::SensorType::Ds18b20, c.hw));
    c.rev = 4;
    return c;
}

// ---------------------------------------------------------------------------
static void test_what_needs_a_restart_and_what_does_not() {
    const NodeConfig a = base();
    NodeConfig b = a;
    CHECK_EQ(classifyChange(a, b), CH_NONE);

    // rev and local are bookkeeping, not settings.
    b.rev = 9; b.local = true;
    CHECK_EQ(classifyChange(a, b), CH_NONE);

    // Live: read from the config where they are used.
    b = a; nodecfg::copyStr(b.name, sizeof(b.name), "attic");
    CHECK_EQ(classifyChange(a, b), CH_LIVE);
    b = a; b.interval_s = 300;
    CHECK_EQ(classifyChange(a, b), CH_LIVE);
    b = a; b.altitude_m = 420.0f;
    CHECK_EQ(classifyChange(a, b), CH_LIVE);
    b = a; b.board = 1;
    CHECK_EQ(classifyChange(a, b), CH_LIVE);
    // The handover's next network is only tried when the current one fails.
    b = a; nodecfg::copyStr(b.net.next.ssid, sizeof(b.net.next.ssid), "home-5g");
    CHECK_EQ(classifyChange(a, b), CH_LIVE);

    // The fields that decide whether the collector is reachable: restart AND
    // run on trial (§4.7).
    const uint8_t trial = CH_RESTART | CH_NET_TRIAL;
    b = a; nodecfg::copyStr(b.net.ssid, sizeof(b.net.ssid), "other");
    CHECK_EQ(classifyChange(a, b), trial);
    b = a; nodecfg::copyStr(b.net.pass, sizeof(b.net.pass), "different1");
    CHECK_EQ(classifyChange(a, b), trial);
    b = a; nodecfg::copyStr(b.net.host, sizeof(b.net.host), "192.168.1.51");
    CHECK_EQ(classifyChange(a, b), trial);
    b = a; b.net.port = 8080;
    CHECK_EQ(classifyChange(a, b), trial);
    b = a; nodecfg::copyStr(b.net.token, sizeof(b.net.token), "tok2");
    CHECK_EQ(classifyChange(a, b), trial);

    // Basic auth: a restart (it also gates the LAN page), but a wrong one is
    // a 401, not "no collector" — not a reason to roll back.
    b = a; nodecfg::copyStr(b.net.basic_pass, sizeof(b.net.basic_pass), "pw");
    CHECK_EQ(classifyChange(a, b), CH_RESTART);

    // Hardware: every driver is brought up once, at boot.
    b = a; b.i2c.sda = 12;
    CHECK_EQ(classifyChange(a, b), CH_RESTART);
    b = a; b.sensors[1].pin = 14;
    CHECK_EQ(classifyChange(a, b), CH_RESTART);
    b = a; b.sensors[0].addr = 0x77;
    CHECK_EQ(classifyChange(a, b), CH_RESTART);
    b = a; b.sensor_count = 1;
    CHECK_EQ(classifyChange(a, b), CH_RESTART);
    // A renamed ds18b20 bus changes the metric names the backlog keeps
    // pointers to; those are fixed for a boot.
    b = a; nodecfg::copyStr(b.sensors[1].metric, sizeof(b.sensors[1].metric), "pool");
    CHECK_EQ(classifyChange(a, b), CH_RESTART);

    // Several at once add up.
    b = a; b.interval_s = 120; b.i2c.scl = 14;
    CHECK_EQ(classifyChange(a, b), CH_LIVE | CH_RESTART);
}

static void test_when_the_report_rides_along() {
    CHECK(shouldSendCfg(false, false));    // first POST after boot
    CHECK(!shouldSendCfg(false, true));    // reported, nothing changed
    CHECK(shouldSendCfg(true, true));      // edited on the page: until adopted
    CHECK(shouldSendCfg(true, false));
}

static ReplyCfg full(uint16_t rev) { ReplyCfg r; r.present = true; r.rev = rev; return r; }
static ReplyCfg revOnly(uint16_t rev) { ReplyCfg r = full(rev); r.revOnly = true; return r; }

static void test_what_to_do_with_the_replys_config() {
    ReplyCfg none;
    CHECK(decideReply(none, 4, false, 0) == ReplyAction::None);
    CHECK(decideReply(none, 4, true, 0) == ReplyAction::None);

    // A newer config: apply it.
    CHECK(decideReply(full(5), 4, false, 0) == ReplyAction::Apply);
    // The one already running: nothing to do.
    CHECK(decideReply(full(4), 4, false, 0) == ReplyAction::None);
    // Refused once: never re-validated, never re-reported.
    CHECK(decideReply(full(5), 4, false, 5) == ReplyAction::Ignore);
    // …but a newer one after it is looked at.
    CHECK(decideReply(full(6), 4, false, 5) == ReplyAction::Apply);
    // A collector never numbers a config 0.
    CHECK(decideReply(full(0), 4, false, 0) == ReplyAction::Ignore);
    CHECK(decideReply(revOnly(0), 4, true, 0) == ReplyAction::Ignore);

    // §3: {"cfg":{"rev":6}} after a local edit — "your local config is now
    // rev 6". Confirm, whatever the rev was.
    CHECK(decideReply(revOnly(6), 4, true, 0) == ReplyAction::Confirm);
    CHECK(decideReply(revOnly(4), 4, true, 0) == ReplyAction::Confirm);
    // Not local and already that rev: an echo, nothing to store.
    CHECK(decideReply(revOnly(4), 4, false, 0) == ReplyAction::None);
    // Not local but a different rev: the collector renumbered what we run.
    CHECK(decideReply(revOnly(7), 4, false, 0) == ReplyAction::Confirm);
    // A rev-only reply is a confirmation, not a config: a refused rev does
    // not block it.
    CHECK(decideReply(revOnly(6), 4, true, 6) == ReplyAction::Confirm);

    // A full config while local: the collector's adoption of it, sent back
    // whole. Applying it is what clears `local`.
    CHECK(decideReply(full(5), 4, true, 0) == ReplyAction::Apply);
    CHECK(decideReply(full(4), 4, true, 0) == ReplyAction::Apply);
}

static void test_a_refusal_is_carried_intact() {
    CfgError e;
    CHECK(!e.pending());
    e.set(5, "sensors[1].pin", "GPIO6 is the SPI flash bus");
    CHECK(e.pending());
    CHECK_EQ(e.rev, 5);
    CHECK_STREQ(e.field, "sensors[1].pin");
    CHECK_STREQ(e.reason, "GPIO6 is the SPI flash bus");
    e.set(7, ROLLBACK_FIELD, ROLLBACK_REASON);
    CHECK_STREQ(e.reason, "rolled back: no collector on new settings");   // §4.7's words
    e.clear();
    CHECK(!e.pending());
    CHECK_STREQ(e.field, "");
}

// ---------------------------------------------------------------------------
static void test_without_a_next_network_only_the_current_one_is_tried() {
    Link l;
    for (int i = 0; i < 10; i++) {
        CHECK(l.networkToTry(false) == Net::Current);
        CHECK_EQ(l.wifiResult(false, Net::Current), LA_NONE);
    }
}

static void test_next_after_two_failed_cycles_then_alternate() {
    Link l;
    // §4.5: "when the current network fails 2 cycles in a row and next is set,
    // try next". Then, while both fail, alternate.
    const Net want[] = { Net::Current, Net::Current, Net::Next, Net::Current,
                         Net::Next, Net::Current, Net::Next };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const Net n = l.networkToTry(true);
        CHECK(n == want[i]);
        CHECK_EQ(l.wifiResult(false, n), LA_NONE);
    }
    // The current one coming back resets the count: no switch.
    Link m;
    m.wifiResult(false, Net::Current);
    CHECK_EQ(m.wifiResult(true, Net::Current), LA_NONE);
    CHECK_EQ(m.wifiFails(), 0);
    CHECK(m.networkToTry(true) == Net::Current);
    m.wifiResult(false, Net::Current);
    CHECK(m.networkToTry(true) == Net::Current);   // one failure, not two in a row
}

static void test_alternation_outlives_the_counter() {
    // The fail count is a u8; hundreds of failed cycles (a day at 60 s, with
    // the old AP gone and the new one not yet up) must not pin the node to
    // one network. Every pair of cycles past NEXT_AFTER_FAILS tries both.
    Link l;
    Net prev = Net::Current;
    for (int i = 0; i < 1000; i++) {
        const Net n = l.networkToTry(true);
        if (i > Link::NEXT_AFTER_FAILS) CHECK(n != prev);
        prev = n;
        CHECK_EQ(l.wifiResult(false, n), LA_NONE);
        CHECK(l.wifiFails() >= (i < 254 ? i + 1 : 254));
    }
    // And joining next after all that still promotes it.
    while (l.networkToTry(true) != Net::Next) l.wifiResult(false, Net::Current);
    CHECK_EQ(l.wifiResult(true, Net::Next), LA_PROMOTE | LA_DISCOVER);
    CHECK_EQ(l.wifiFails(), 0);
}

static void test_joining_next_promotes_it_and_looks_for_the_collector() {
    Link l;
    l.wifiResult(false, Net::Current);
    l.wifiResult(false, Net::Current);
    CHECK(l.networkToTry(true) == Net::Next);
    const uint8_t a = l.wifiResult(true, Net::Next);
    CHECK(a & LA_PROMOTE);
    CHECK(a & LA_DISCOVER);          // right after a network switch, §3.1
    CHECK_EQ(l.wifiFails(), 0);
    CHECK(l.networkToTry(true) == Net::Current);   // the promoted one is current now

    NodeConfig c = base();
    nodecfg::copyStr(c.net.next.ssid, sizeof(c.net.next.ssid), "home-5g");
    nodecfg::copyStr(c.net.next.pass, sizeof(c.net.next.pass), "newpass99");
    promoteNext(c);
    CHECK_STREQ(c.net.ssid, "home-5g");
    CHECK_STREQ(c.net.pass, "newpass99");
    // The old one is kept as next, so a cancelled switch is survivable.
    CHECK_STREQ(c.net.next.ssid, "home");
    CHECK_STREQ(c.net.next.pass, "hunter22");
    CHECK(c.local);                  // the collector learns of it from the report
    // And back again, if the collector cancels and this node follows.
    promoteNext(c);
    CHECK_STREQ(c.net.ssid, "home");
    CHECK_STREQ(c.net.next.ssid, "home-5g");
}

static void test_discovery_after_three_failed_posts() {
    Link l;
    CHECK_EQ(l.postResult(false), LA_NONE);
    CHECK_EQ(l.postResult(false), LA_NONE);
    CHECK_EQ(l.postResult(false), LA_DISCOVER);
    // Counted afresh: a collector that is simply off costs one broadcast every
    // third failure, not every one.
    CHECK_EQ(l.postResult(false), LA_NONE);
    CHECK_EQ(l.postResult(false), LA_NONE);
    CHECK_EQ(l.postResult(false), LA_DISCOVER);
    // An answer in between resets it.
    l.postResult(false);
    l.postResult(false);
    CHECK_EQ(l.postResult(true), LA_NONE);
    CHECK_EQ(l.postFails(), 0);
    CHECK_EQ(l.postResult(false), LA_NONE);
}

static void test_a_network_change_that_reaches_nothing_is_rolled_back() {
    Link l;
    l.beginTrial();
    CHECK(l.onTrial());
    // §4.7: five cycles without an answered POST. WiFi failing counts the
    // same as POSTs failing — both are "no collector on new settings".
    for (int i = 0; i < 4; i++) {
        l.postResult(false);
        CHECK_EQ(l.cycleEnd(), LA_NONE);
    }
    CHECK_EQ(l.trialCycles(), 4);
    CHECK_EQ(l.cycleEnd(), LA_ROLLBACK);
    CHECK(!l.onTrial());
    // Once rolled back, nothing more is counted.
    CHECK_EQ(l.cycleEnd(), LA_NONE);
}

static void test_one_answer_keeps_the_new_settings() {
    Link l;
    l.beginTrial();
    CHECK_EQ(l.cycleEnd(), LA_NONE);
    CHECK_EQ(l.cycleEnd(), LA_NONE);
    CHECK_EQ(l.cycleEnd(), LA_NONE);
    // Found on the fourth cycle — say by discovery, after the host moved.
    CHECK_EQ(l.postResult(true), LA_TRIAL_OK);
    CHECK(!l.onTrial());
    CHECK_EQ(l.cycleEnd(), LA_NONE);
    for (int i = 0; i < 10; i++) CHECK_EQ(l.cycleEnd(), LA_NONE);
    // A second answer is not a second "trial passed".
    CHECK_EQ(l.postResult(true), LA_NONE);
}

static void test_the_trial_is_counted_per_cycle_and_afresh_after_a_restart() {
    // An answer in one cycle says nothing about the next: the flag is per
    // cycle, so it cannot leak forward and excuse a silent cycle.
    Link l;
    CHECK_EQ(l.postResult(true), LA_NONE);   // not on trial yet
    CHECK_EQ(l.cycleEnd(), LA_NONE);
    l.beginTrial();
    for (int i = 0; i < 4; i++) CHECK_EQ(l.cycleEnd(), LA_NONE);
    CHECK_EQ(l.cycleEnd(), LA_ROLLBACK);     // the fifth silent cycle, not the sixth

    // The trial survives a restart in /sync.json; the count does not, so a
    // node that reboots mid-trial gets five fresh cycles.
    Link m;
    m.beginTrial();
    m.cycleEnd(); m.cycleEnd();
    CHECK_EQ(m.trialCycles(), 2);
    Link afterRestart;
    afterRestart.beginTrial();
    CHECK_EQ(afterRestart.trialCycles(), 0);
}

static void test_a_whole_handover() {
    // The collector moved to "home-5g" and told this node in net.next; the
    // old network then disappeared.
    NodeConfig c = base();
    nodecfg::copyStr(c.net.next.ssid, sizeof(c.net.next.ssid), "home-5g");
    Link l;
    int cycle = 0;
    bool moved = false;
    while (cycle < 10 && !moved) {
        cycle++;
        const Net n = l.networkToTry(c.net.next.ssid[0] != '\0');
        const bool up = (n == Net::Next);        // only the new network exists
        const uint8_t a = l.wifiResult(up, n);
        if (a & LA_PROMOTE) { promoteNext(c); moved = true; }
        l.cycleEnd();
    }
    CHECK(moved);
    CHECK_EQ(cycle, 3);                          // two failed cycles, then next
    CHECK_STREQ(c.net.ssid, "home-5g");
    CHECK(c.local);
}

// ---------------------------------------------------------------------------
static void test_metric_names_outlive_the_reading() {
    NameTable t;
    char buf[16];
    nodecfg::copyStr(buf, sizeof(buf), "temperature");
    const char* p = t.intern(buf);
    CHECK(p != nullptr);
    CHECK(p != buf);                       // a copy, not the caller's buffer
    nodecfg::copyStr(buf, sizeof(buf), "overwritten");
    CHECK_STREQ(p, "temperature");         // …which is the point
    CHECK(t.intern("temperature") == p);   // the same name, the same pointer
    CHECK_EQ(t.size(), 1);
    CHECK(t.intern("") == nullptr);
    CHECK(t.intern(nullptr) == nullptr);

    // Full: refuse rather than hand back something that will change.
    NameTable f;
    char n[16];
    for (unsigned i = 0; i < NameTable::CAP; i++) {
        n[0] = '\0';
        nodecfg::strAppend(n, sizeof(n), "probe_temp_");
        nodecfg::strAppendUint(n, sizeof(n), i);
        CHECK(f.intern(n) != nullptr);
    }
    CHECK(f.intern("one_too_many") == nullptr);
    CHECK(f.intern("probe_temp_3") != nullptr);   // known names still resolve
    // The longest name the collector stores fits whole.
    NameTable g;
    CHECK_STREQ(g.intern("abcdefghij_7xyz"), "abcdefghij_7xyz");
}

// ---------------------------------------------------------------------------
// sameSensor() compares per_pulse, and the value it compares has been through
// JSON: the collector's reply is its own decode of this node's report,
// re-encoded. ArduinoJson writes a float with six decimal places, so the FIRST
// trip can round (0.00123457 goes out as 0.001235) — and it does not always
// read its own text back to the same float either, so a value keeps drifting
// by a few units in the last place on later trips too (about 1% of floats;
// 4.122726 → 4.12272549 → 4.12272501). With == that drift read as a sensor
// change and restarted the node on a config that only renamed it. A node's
// running per_pulse has made the first trip whenever it came from a file
// (/config.json) or a reply, so an unchanged sensor must not read as changed.

/// `from` → JSON text → decoded over `into`, the way every hop does it.
static NodeConfig viaJson(const NodeConfig& from, const NodeConfig& into, uint8_t encFlags) {
    char buf[2048];
    const size_t n = nodecfg::encodeConfigTo(from, buf, sizeof(buf), encFlags);
    CHECK(n > 0);
    JsonDocument doc;
    CHECK(deserializeJson(doc, buf, n) == DeserializationError::Ok);
    NodeConfig out = into;
    nodecfg::Issue is;
    CHECK(nodecfg::decodeConfig(doc.as<JsonVariantConst>(), out, nodecfg::NCJ_DEC_REV, &is));
    return out;
}

static NodeConfig withPulse(float perPulse) {
    NodeConfig c = base();
    nodecfg::SensorCfg p = nodecfg::sensorDefaults(nodecfg::SensorType::Pulse, c.hw);
    nodecfg::pulseModeDefaults(p, nodecfg::PulseMode::Flow);
    p.pin = 14;
    p.per_pulse = perPulse;
    nodecfg::addSensor(c, p);
    return c;
}

static void test_a_json_round_trip_is_not_a_sensor_change() {
    // Spread over the whole range a per_pulse can sensibly take and well past
    // it, in both notations ArduinoJson uses (plain, and e-notation below 1e-5
    // and from 1e7), plus the values people actually type.
    const float picked[] = { 0.2794f, 0.00222f, 0.00123457f, 0.1f, 1.0f / 3.0f,
                             2.25e-3f, 1.2345678e-5f, 9.87654e-6f, 123.456789f,
                             4.5e7f };
    int tried = 0, rounded = 0, drifted = 0, misread = 0;
    auto one = [&](float pp) {
        const NodeConfig typed = withPulse(pp);
        // Saved and loaded: the node's running config after any restart.
        const NodeConfig running = viaJson(typed, typed, nodecfg::NCJ_SECRETS);
        if (running.sensors[2].per_pulse != pp) rounded++;
        // Reported (no secrets), decoded by the collector, renamed there and
        // sent back in a reply: nothing about the sensor changed.
        NodeConfig atCollector = viaJson(running, typed, 0);
        nodecfg::copyStr(atCollector.name, sizeof(atCollector.name), "attic");
        const NodeConfig reply = viaJson(atCollector, running, nodecfg::NCJ_SECRETS);
        if (reply.sensors[2].per_pulse != running.sensors[2].per_pulse) drifted++;
        if (NodeSync::classifyChange(running, reply) != NodeSync::CH_LIVE) misread++;
        // And that reply, applied, saved and loaded again, is still the same.
        const NodeConfig again = viaJson(reply, reply, nodecfg::NCJ_SECRETS);
        if (NodeSync::classifyChange(reply, again) != NodeSync::CH_NONE) misread++;
        tried++;
    };
    for (float pp : picked) one(pp);
    // Every 9973rd float bit pattern from 1e-9 to 1e9.
    const float lo = 1e-9f, hi = 1e9f;
    uint32_t blo, bhi;
    memcpy(&blo, &lo, 4);
    memcpy(&bhi, &hi, 4);
    for (uint32_t bits = blo; bits <= bhi; bits += 9973) {
        float pp;
        memcpy(&pp, &bits, 4);
        one(pp);
    }
    printf("  per_pulse: %d values, %d rounded on the first trip, %d drifted after it, "
           "%d misread as a change\n", tried, rounded, drifted, misread);
    CHECK(tried > 50000);
    CHECK(rounded > 0);          // the first trip does round,
    CHECK(drifted > 0);          // later ones drift: the premise is real
    CHECK_EQ(misread, 0);        // ...and neither is a sensor change

    // The tolerance is not a blind spot for a real edit.
    CHECK(samePerPulse(0.2794f, 0.2794f));
    CHECK(!samePerPulse(0.2794f, 0.2795f));
    CHECK(!samePerPulse(0.00222f, 0.00223f));
    CHECK(!samePerPulse(1e-8f, 2e-8f));
    CHECK(!samePerPulse(0.2794f, 0.2794f * 1.00001f));   // ten parts per million
    CHECK(samePerPulse(4.122726f, 4.12272501f));         // the drift above

    // The one case that is not covered: a value that never made the first
    // trip — a compiled-in PULSE_UNITS_PER_PULSE with more than six decimals,
    // on a node running without a /config.json. It reads as a change once, and
    // the node restarts once (the safe direction: a restart applies anything),
    // after which it runs on the saved, rounded value like every other node.
    const NodeConfig seeded = withPulse(0.00123457f);
    const NodeConfig echo = viaJson(viaJson(seeded, seeded, 0), seeded, nodecfg::NCJ_SECRETS);
    CHECK(NodeSync::classifyChange(seeded, echo) & NodeSync::CH_RESTART);
}

int main() {
    RUN(test_what_needs_a_restart_and_what_does_not);
    RUN(test_when_the_report_rides_along);
    RUN(test_what_to_do_with_the_replys_config);
    RUN(test_a_refusal_is_carried_intact);
    RUN(test_without_a_next_network_only_the_current_one_is_tried);
    RUN(test_next_after_two_failed_cycles_then_alternate);
    RUN(test_alternation_outlives_the_counter);
    RUN(test_joining_next_promotes_it_and_looks_for_the_collector);
    RUN(test_discovery_after_three_failed_posts);
    RUN(test_a_network_change_that_reaches_nothing_is_rolled_back);
    RUN(test_one_answer_keeps_the_new_settings);
    RUN(test_the_trial_is_counted_per_cycle_and_afresh_after_a_restart);
    RUN(test_a_whole_handover);
    RUN(test_metric_names_outlive_the_reading);
    RUN(test_a_json_round_trip_is_not_a_sensor_change);
    return SUMMARY();
}
