// Host unit tests for src/nodes/NodeCfgRules.h — the collector's decisions
// about node configuration (docs/NODE_CONFIG.md §2–§5, §7).
//
// The firmware around these rules is files, radio frames and HTTP; the rules
// are what goes wrong silently. Each test below pins one of them:
//
//   • revs never wrap to 0, and a node that caught up is applied;
//   • a rejected rev is never re-sent (every refusal is battery);
//   • keys and file names cannot escape /nodes/;
//   • a node's own edit wins, a node ahead of the collector is believed, and
//     a node behind is simply pending;
//   • a secret the collector never saw is sent as "keep yours", never as "";
//   • an ESP-NOW node's first report keeps the table's label and interval;
//   • a handover is ready only when every node runs the rev that carried the
//     next network, and never switches before the page saw a status;
//   • a node's local edit during a handover keeps the next network and is
//     sent it again before it counts as ready;
//   • a new rev moves past any rev the node was told, not only the desired
//     one; a second rename and a leftover *.tmp do not lose or double a node;
//   • DATA2 ids come out under the same names the node lists, non-finite
//     values dropped.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "src/nodes/NodeCfgRules.h"
#include "src/nodecfg/NodeConfigJson.h"
#include "check.h"

using namespace nodecfg;
using namespace ncr;

static NodeConfig wifiNode() {
    NodeConfig c = configDefaults(Transport::Wifi, Hw::Esp8266);
    copyStr(c.name, sizeof(c.name), "balcony");
    copyStr(c.net.ssid, sizeof(c.net.ssid), "home");
    copyStr(c.net.pass, sizeof(c.net.pass), "correct horse");
    copyStr(c.net.host, sizeof(c.net.host), "192.168.1.50");
    copyStr(c.net.token, sizeof(c.net.token), "tok-123");
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp8266));
    return c;
}

static NodeConfig espnowNode() {
    NodeConfig c = configDefaults(Transport::EspNow, Hw::Esp32c3);
    copyStr(c.name, sizeof(c.name), "node-A1B2");
    addSensor(c, sensorDefaults(SensorType::Bmx280, Hw::Esp32c3));
    return c;
}

// ===========================================================================
// Status and revs
// ===========================================================================

static void test_status_names_round_trip() {
    const uint8_t all[] = { ST_APPLIED, ST_PENDING, ST_REJECTED };
    for (uint8_t s : all) CHECK_EQ((int)statusParse(statusName(s)), (int)s);
    CHECK(statusName(ST_NONE) == nullptr);        // never written to a file
    CHECK_EQ((int)statusParse(nullptr), (int)ST_NONE);
    CHECK_EQ((int)statusParse("Applied"), (int)ST_NONE);
    CHECK_EQ((int)statusParse(""), (int)ST_NONE);
}

static void test_next_rev_skips_zero() {
    CHECK_EQ(nextRev(0), 1);
    CHECK_EQ(nextRev(41), 42);
    CHECK_EQ(nextRev(0xFFFE), 0xFFFF);
    CHECK_EQ(nextRev(0xFFFF), 1);                 // 0 means "never synced"
}

static void test_status_after_applied() {
    CHECK_EQ((int)statusAfterApplied(ST_PENDING, 5, 5), (int)ST_APPLIED);
    CHECK_EQ((int)statusAfterApplied(ST_REJECTED, 5, 6), (int)ST_APPLIED);
    CHECK_EQ((int)statusAfterApplied(ST_PENDING, 5, 4), (int)ST_PENDING);
    // Still behind a rejected rev: it stays rejected — the node is (rightly)
    // running its previous config.
    CHECK_EQ((int)statusAfterApplied(ST_REJECTED, 5, 4), (int)ST_REJECTED);
}

static void test_should_send_only_pending_and_behind() {
    CHECK(shouldSend(ST_PENDING, 5, 4));
    CHECK(shouldSend(ST_PENDING, 5, 0));
    CHECK(!shouldSend(ST_PENDING, 5, 5));
    CHECK(!shouldSend(ST_APPLIED, 5, 4));
    CHECK(!shouldSend(ST_REJECTED, 5, 4));        // not again until the next bump
    CHECK(!shouldSend(ST_NONE, 5, 4));
}

// nextRev() wraps 0xFFFF -> 1. Compared as plain numbers, the rev after the
// wrap would be "older" than the one before it: never sent, and the node
// called applied. Every ordering the collector makes goes through
// revAtOrPast(), which reads the difference as signed.
static void test_revs_compare_across_the_wrap() {
    CHECK(revAtOrPast(5, 5));
    CHECK(revAtOrPast(6, 5));
    CHECK(!revAtOrPast(4, 5));
    CHECK(revAtOrPast(1, 0xFFFF));                // one bump past the wrap
    CHECK(revAtOrPast(3, 0xFFFE));
    CHECK(!revAtOrPast(0xFFFF, 1));
    CHECK(!revAtOrPast(0, 1));                    // never synced: behind all
    CHECK(!revAtOrPast(0, 0xFFFF));
    CHECK(revAtOrPast(0x8000, 0));
    CHECK(revAtOrPast(0, 0));
    CHECK(revAtOrPast(40000, 20000));             // far apart, same side

    // The decisions built on it, at the wrap: desired rev 1, node on 65535.
    const uint16_t desired = nextRev(0xFFFF);
    CHECK(shouldSend(ST_PENDING, desired, 0xFFFF));
    CHECK_EQ((int)statusAfterApplied(ST_PENDING, desired, 0xFFFF), (int)ST_PENDING);
    CHECK_EQ((int)statusAfterApplied(ST_PENDING, desired, desired), (int)ST_APPLIED);
    CHECK(!shouldSend(ST_PENDING, desired, desired));
    ReportPlan p = planReport(true, desired, 0xFFFF, false);   // node behind: not adopted
    CHECK(!p.adopt);
    CHECK_EQ(p.applied, 0xFFFF);
    p = planReport(true, 0xFFFF, 2, false);                    // node past the wrap: ahead
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 2);
}

// ===========================================================================
// Keys and file names
// ===========================================================================

static void test_valid_name() {
    CHECK(validName("balcony"));
    CHECK(validName("node-A1_b2"));
    CHECK(validName("abcdefghijklmnop"));         // 16
    CHECK(!validName("abcdefghijklmnopq"));       // 17
    CHECK(!validName(""));
    CHECK(!validName(nullptr));
    CHECK(!validName("../x"));
    CHECK(!validName("a/b"));
    CHECK(!validName("a b"));
    CHECK(!validName("a.json"));
}

static void test_parse_key() {
    bool espnow = true;
    char name[NODE_NAME_MAX + 1];
    uint8_t id = 9;

    CHECK(parseKey("w:balcony", espnow, name, id));
    CHECK(!espnow);
    CHECK_STREQ(name, "balcony");
    CHECK_EQ((int)id, 0);

    CHECK(parseKey("e:3", espnow, name, id));
    CHECK(espnow);
    CHECK_EQ((int)id, 3);
    CHECK_STREQ(name, "");
    CHECK(parseKey("e:254", espnow, name, id));
    CHECK_EQ((int)id, 254);

    const char* bad[] = { "e:0", "e:03", "e:255", "e:1000", "e:", "e:1a", "e:-1",
                          "w:", "w:../x", "w:abcdefghijklmnopq", "x:1", "w", "", "wbalcony",
                          "w:a b" };
    for (const char* k : bad) {
        const bool ok = parseKey(k, espnow, name, id);
        if (ok) std::printf("  accepted bad key \"%s\"\n", k);
        CHECK(!ok);
    }
    CHECK(!parseKey(nullptr, espnow, name, id));
}

static void test_format_key_and_path() {
    char key[KEY_CAP];
    char path[PATH_CAP];
    formatKey(key, false, "balcony", 0);
    CHECK_STREQ(key, "w:balcony");
    formatKey(key, true, nullptr, 17);
    CHECK_STREQ(key, "e:17");

    filePath(path, false, "abcdefghijklmnop", 0);        // the longest name fits
    CHECK_STREQ(path, "/nodes/w_abcdefghijklmnop.json");
    filePath(path, true, nullptr, 254);
    CHECK_STREQ(path, "/nodes/e_254.json");

    // Every key the parser accepts formats back to itself.
    const char* keys[] = { "w:balcony", "w:abcdefghijklmnop", "e:1", "e:254" };
    for (const char* k : keys) {
        bool espnow = false;
        char name[NODE_NAME_MAX + 1];
        uint8_t id = 0;
        CHECK(parseKey(k, espnow, name, id));
        formatKey(key, espnow, name, id);
        CHECK_STREQ(key, k);
    }
}

// ===========================================================================
// Reports (§0.4, §3, §5)
// ===========================================================================

static void test_plan_report_no_config_held() {
    ReportPlan p = planReport(false, 0, 0, false);       // never synced
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 1);
    CHECK_EQ(p.applied, 1);

    p = planReport(false, 0, 7, false);                  // collector lost its file
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 7);
    CHECK_EQ(p.applied, 7);

    p = planReport(false, 0, 7, true);                   // a local edit: one past
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 8);
    CHECK_EQ(p.applied, 8);
}

static void test_plan_report_local_edit_wins() {
    ReportPlan p = planReport(true, 9, 4, true);         // collector ahead: still wins
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 10);
    CHECK_EQ(p.applied, 10);

    p = planReport(true, 4, 9, true);
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 10);

    p = planReport(true, 0xFFFF, 0xFFFE, true);          // wraps past 0
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 1);

    p = planReport(true, 0xFFFF, 3, true);               // 3 is past the wrap: the later one
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 4);
}

static void test_plan_report_status_only() {
    ReportPlan p = planReport(true, 5, 9, false);        // node ahead: believed
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 9);
    CHECK_EQ(p.applied, 9);

    p = planReport(true, 5, 5, false);                   // in step
    CHECK(!p.adopt);
    CHECK_EQ(p.rev, 5);
    CHECK_EQ(p.applied, 5);

    p = planReport(true, 5, 0, false);                   // node reset: pending, gets it back
    CHECK(!p.adopt);
    CHECK_EQ(p.rev, 5);
    CHECK_EQ(p.applied, 0);
    CHECK(shouldSend(statusAfterApplied(ST_PENDING, p.rev, p.applied), p.rev, p.applied));
}

/// §5: every complete ESP-NOW CFG_REPORT is answered, the one after a boot as
/// well as a local edit, and the node keeps reporting until it hears it. What
/// the answer says must not move a node that did not edit anything.
static void test_report_answer_rev() {
    // A local edit is told the rev it is adopted at.
    ReportPlan p = planReport(true, 9, 4, true);
    CHECK_EQ(reportAnswerRev(p, 4, true), 10);
    p = planReport(false, 0, 0, true);
    CHECK_EQ(reportAnswerRev(p, 0, true), 1);

    // After a boot, every case is told its own rev back — never a rev taken
    // from what the collector adopted:
    p = planReport(false, 0, 0, false);            // unknown, never synced
    CHECK(p.adopt);
    CHECK_EQ(p.rev, 1);                            // adopted at 1 ...
    CHECK_EQ(reportAnswerRev(p, 0, false), 0);     // ... the node stays at 0, and fetches
    p = planReport(false, 0, 7, false);            // unknown, collector lost its file
    CHECK_EQ(reportAnswerRev(p, 7, false), 7);
    p = planReport(true, 5, 9, false);             // known, node ahead: believed
    CHECK(p.adopt);
    CHECK_EQ(reportAnswerRev(p, 9, false), 9);
    p = planReport(true, 5, 5, false);             // in step
    CHECK_EQ(reportAnswerRev(p, 5, false), 5);
    p = planReport(true, 5, 2, false);             // behind the desired rev
    CHECK(!p.adopt);                               // not adopted as desired
    CHECK_EQ(reportAnswerRev(p, 2, false), 2);     // told 2: still pending
    CHECK(shouldSend(statusAfterApplied(ST_PENDING, p.rev, 2), p.rev, 2));

    // The same report heard twice (the answer was lost, the node reports
    // again on a later wake) is a status report the second time: once the
    // first adopted it, the collector holds that rev and the repeat is in
    // step or behind — never adopted twice.
    p = planReport(false, 0, 7, false);
    ReportPlan again = planReport(true, p.rev, 7, false);
    CHECK(!again.adopt);
    CHECK_EQ(reportAnswerRev(again, 7, false), 7);
    p = planReport(false, 0, 0, false);
    again = planReport(true, p.rev, 0, false);
    CHECK(!again.adopt);
    CHECK_EQ(reportAnswerRev(again, 0, false), 0);
}

static void test_ingest_reply() {
    CHECK_EQ((int)ingestReply(true, ST_APPLIED, 6, 6, 0), (int)REPLY_REV);
    CHECK_EQ((int)ingestReply(false, ST_PENDING, 6, 5, 0), (int)REPLY_FULL);
    CHECK_EQ((int)ingestReply(false, ST_PENDING, 6, 6, 0), (int)REPLY_NONE);
    CHECK_EQ((int)ingestReply(false, ST_APPLIED, 6, 5, 0), (int)REPLY_NONE);
    CHECK_EQ((int)ingestReply(false, ST_REJECTED, 6, 5, 0), (int)REPLY_NONE);
    // The very POST that refuses rev 6 is not answered with rev 6 again...
    CHECK_EQ((int)ingestReply(false, ST_PENDING, 6, 5, 6), (int)REPLY_NONE);
    // ...but an older refusal does not hold back a newer rev.
    CHECK_EQ((int)ingestReply(false, ST_PENDING, 7, 5, 6), (int)REPLY_FULL);
}

// ===========================================================================
// Secrets (§0.5)
// ===========================================================================

static void test_secret_bits() {
    NodeConfig a = wifiNode();
    NodeConfig b = a;
    CHECK_EQ((int)secretsDiff(a, b), 0);
    CHECK_EQ((int)secretsSet(a), (int)(SEC_PASS | SEC_TOKEN));

    copyStr(b.net.pass, sizeof(b.net.pass), "other");
    copyStr(b.net.next.pass, sizeof(b.net.next.pass), "nextpass");
    CHECK_EQ((int)secretsDiff(a, b), (int)(SEC_PASS | SEC_NPASS));

    // An edit that cleared the token and set the next pass.
    uint8_t known = SEC_PASS | SEC_TOKEN, dirty = 0;
    NodeConfig now = a;
    now.net.token[0] = '\0';
    copyStr(now.net.next.pass, sizeof(now.net.next.pass), "nextpass");
    secretsEdited(secretsDiff(a, now), now, known, dirty);
    CHECK_EQ((int)dirty, (int)(SEC_TOKEN | SEC_NPASS));
    CHECK_EQ((int)known, (int)(SEC_PASS | SEC_NPASS));
}

static void test_report_secrets_known() {
    JsonDocument d;
    deserializeJson(d, "{\"net\":{\"pass\":\"\",\"pass_set\":true,\"token_set\":false,"
                       "\"next\":{\"ssid\":\"n\",\"pass_set\":true}}}");
    CHECK_EQ((int)reportSecretsKnown(d.as<JsonObjectConst>()), (int)(SEC_PASS | SEC_NPASS));

    deserializeJson(d, "{\"name\":\"x\"}");                 // ESP-NOW: no net at all
    CHECK_EQ((int)reportSecretsKnown(d.as<JsonObjectConst>()), 0);
}

static void test_secrets_for_node_sends_only_dirty() {
    NodeConfig c = wifiNode();
    copyStr(c.net.next.ssid, sizeof(c.net.next.ssid), "newnet");
    copyStr(c.net.next.pass, sizeof(c.net.next.pass), "nextpass");
    JsonDocument d;
    encodeConfig(c, d.to<JsonObject>(), NCJ_SECRETS);
    secretsForNode(d.as<JsonObject>(), SEC_NPASS);

    // The one the collector set travels...
    CHECK_STREQ(d["net"]["next"]["pass"].as<const char*>(), "nextpass");
    // ...the ones it only holds a (possibly stale) copy of say "keep yours".
    CHECK_STREQ(d["net"]["pass"].as<const char*>(), "");
    CHECK(d["net"]["pass_set"].as<bool>());
    CHECK_STREQ(d["net"]["token"].as<const char*>(), "");
    CHECK(d["net"]["token_set"].as<bool>());
    CHECK_STREQ(d["net"]["basic_pass"].as<const char*>(), "");
    CHECK(d["net"]["basic_pass_set"].as<bool>());

    // And the node's decoder reads "" + _set as keep: a node holding its own
    // passphrase decodes this and still has it.
    NodeConfig onNode = wifiNode();
    copyStr(onNode.net.pass, sizeof(onNode.net.pass), "typed on the node");
    Issue iss;
    CHECK(decodeConfig(d.as<JsonObjectConst>(), onNode, NCJ_DEC_REV | NCJ_DEC_IDENTITY, &iss));
    CHECK_STREQ(onNode.net.pass, "typed on the node");
    CHECK_STREQ(onNode.net.next.pass, "nextpass");
    CHECK_STREQ(onNode.net.next.ssid, "newnet");

    // An ESP-NOW document has no net and is left alone.
    JsonDocument e;
    encodeConfig(espnowNode(), e.to<JsonObject>(), NCJ_SECRETS);
    secretsForNode(e.as<JsonObject>(), 0);
    CHECK(e["net"].isNull());
}

static void test_secrets_for_get_reports_knowledge() {
    NodeConfig c = wifiNode();                           // holds pass + token
    JsonDocument d;
    encodeConfig(c, d.to<JsonObject>(), 0);
    secretsForGet(d.as<JsonObject>(), SEC_BPASS);        // what the node said it has
    CHECK(!d["net"]["pass_set"].as<bool>());
    CHECK(!d["net"]["token_set"].as<bool>());
    CHECK(d["net"]["basic_pass_set"].as<bool>());
    CHECK(!d["net"]["next"]["pass_set"].as<bool>());
    CHECK_STREQ(d["net"]["pass"].as<const char*>(), "");
    CHECK_STREQ(d["net"]["token"].as<const char*>(), "");
}

// ===========================================================================
// ESP-NOW first contact
// ===========================================================================

static void test_adopt_table_identity() {
    NodeConfig c = espnowNode();
    c.interval_s = 300;
    CHECK(adoptTableIdentity(c, "garden", 600));
    CHECK_STREQ(c.name, "garden");
    CHECK_EQ(c.interval_s, 600);

    CHECK(!adoptTableIdentity(c, "garden", 600));        // already the same: no bump

    // A label that is not a §1 name cannot be sent; an interval below the
    // minimum (0 = table has none) is not one.
    CHECK(!adoptTableIdentity(c, "my garden", 0));
    CHECK_STREQ(c.name, "garden");
    CHECK_EQ(c.interval_s, 600);
    CHECK(!adoptTableIdentity(c, nullptr, INTERVAL_MIN_S - 1));
    CHECK(adoptTableIdentity(c, nullptr, INTERVAL_MIN_S));
    CHECK_EQ(c.interval_s, INTERVAL_MIN_S);
}

// ===========================================================================
// Handover (§4)
// ===========================================================================

static void test_handover_classify() {
    CHECK_EQ((int)hoClassify(true, 7, 7, false), (int)HO_READY);
    CHECK_EQ((int)hoClassify(true, 8, 7, true), (int)HO_READY);   // ready beats offline
    CHECK_EQ((int)hoClassify(true, 6, 7, false), (int)HO_PENDING);
    CHECK_EQ((int)hoClassify(true, 6, 7, true), (int)HO_OFFLINE);
    CHECK_EQ((int)hoClassify(false, 0, 0, false), (int)HO_PENDING);   // no file: never ready
    CHECK_EQ((int)hoClassify(false, 0, 0, true), (int)HO_OFFLINE);
    CHECK_EQ((int)hoClassify(true, 5, 0, false), (int)HO_PENDING);    // not handed one yet
    CHECK_EQ((int)hoClassify(true, 1, 0xFFFF, false), (int)HO_READY);  // across the wrap
    CHECK_EQ((int)hoClassify(true, 0xFFFF, 1, false), (int)HO_PENDING);
}

static void test_handover_auto_switch() {
    CHECK(hoAutoSwitch(true, 0, HO_MIN_MS));
    CHECK(!hoAutoSwitch(true, 0, HO_MIN_MS - 1));        // the page sees a status first
    CHECK(!hoAutoSwitch(true, 1, 60000));
    CHECK(!hoAutoSwitch(false, 0, 60000));
}

static void test_handover_apply_next() {
    NodeConfig w = wifiNode();
    CHECK_EQ((int)hoApplyNext(w, "newnet", "pw"), (int)SEC_NPASS);
    CHECK_STREQ(w.net.next.ssid, "newnet");
    CHECK_STREQ(w.net.next.pass, "pw");
    CHECK_EQ((int)hoApplyNext(w, "newnet2", "pw"), 0);  // same pass: not re-sent
    CHECK_STREQ(w.net.next.ssid, "newnet2");
    CHECK_EQ((int)hoApplyNext(w, "", nullptr), (int)SEC_NPASS);   // cancel clears it
    CHECK_STREQ(w.net.next.ssid, "");
    CHECK_STREQ(w.net.next.pass, "");

    NodeConfig e = espnowNode();
    CHECK_EQ((int)hoApplyNext(e, "newnet", "pw"), 0);   // ESP-NOW never needs the pass
    CHECK_STREQ(e.link.next_ssid, "newnet");
    CHECK_EQ((int)hoApplyNext(e, "", nullptr), 0);
    CHECK_STREQ(e.link.next_ssid, "");
}

// ---------------------------------------------------------------------------
// A handover survives a node's local edit
// ---------------------------------------------------------------------------

/// `node`'s own report of itself (no secrets, as it sends it), decoded over
/// `desired` the way the store adopts it.
static NodeConfig adoptOver(const NodeConfig& desired, const NodeConfig& node) {
    JsonDocument doc;
    encodeConfig(node, doc.to<JsonObject>(), 0);
    doc["local"] = true;
    NodeConfig c = desired;
    CHECK(decodeConfig(doc.as<JsonObjectConst>(), c, NCJ_DEC_REV | NCJ_DEC_IDENTITY, nullptr));
    return c;
}

static void test_handover_keeps_next_over_a_local_edit() {
    // Desired rev 7 carries the next network (the handover rev); the node
    // runs 7, then edits its interval locally — its report has no next.
    NodeConfig desired = wifiNode();
    hoApplyNext(desired, "newnet", "newpass1");
    desired.rev = 7;
    NodeConfig node = wifiNode();
    node.interval_s = 120;
    node.rev = 7;

    const ReportPlan p = planReport(true, 7, 7, true);
    CHECK_EQ((int)p.rev, 8);
    NodeConfig c = adoptOver(desired, node);
    CHECK_STREQ(c.net.next.ssid, "");              // what the report alone does
    CHECK(hoKeepNext(c, desired, "newnet"));
    CHECK_STREQ(c.net.next.ssid, "newnet");
    CHECK_STREQ(c.net.next.pass, "newpass1");
    CHECK_EQ((int)c.interval_s, 120);              // the node's edit still wins
    // The node is told 8 and runs it; the next network goes out in 9, which
    // is the rev it must run to be ready — not 8.
    const uint16_t hoRev = revAfter(p.rev, p.applied);
    CHECK_EQ((int)hoRev, 9);
    CHECK_EQ((int)hoClassify(true, p.applied, hoRev, false), (int)HO_PENDING);
    CHECK_EQ((int)hoClassify(true, hoRev, hoRev, false), (int)HO_READY);

    // A report that still carries it ("keep yours" for the pass): nothing.
    NodeConfig has = node;
    hoApplyNext(has, "newnet", "");
    NodeConfig c2 = desired;
    {
        JsonDocument doc;
        encodeConfig(has, doc.to<JsonObject>(), 0);
        doc["net"]["next"]["pass_set"] = true;
        CHECK(decodeConfig(doc.as<JsonObjectConst>(), c2, NCJ_DEC_REV | NCJ_DEC_IDENTITY, nullptr));
    }
    CHECK(!hoKeepNext(c2, desired, "newnet"));
    CHECK_STREQ(c2.net.next.pass, "newpass1");

    // A node that already moved to the new network itself: left alone.
    NodeConfig moved = node;
    copyStr(moved.net.ssid, sizeof(moved.net.ssid), "newnet");
    hoApplyNext(moved, "home", "");
    NodeConfig c3 = adoptOver(desired, moved);
    CHECK(!hoKeepNext(c3, desired, "newnet"));

    // ESP-NOW: link.next_ssid.
    NodeConfig ed = espnowNode();
    hoApplyNext(ed, "newnet", nullptr);
    NodeConfig en = espnowNode();
    en.interval_s = 900;
    NodeConfig ec = adoptOver(ed, en);
    CHECK_STREQ(ec.link.next_ssid, "");
    CHECK(hoKeepNext(ec, ed, "newnet"));
    CHECK_STREQ(ec.link.next_ssid, "newnet");
    CHECK(!hoKeepNext(ec, ed, "newnet"));          // now it matches
}

// ---------------------------------------------------------------------------
// A new rev never reuses one the node already holds
// ---------------------------------------------------------------------------

static void test_rev_after_moves_past_what_the_node_was_told() {
    CHECK_EQ((int)revAfter(5, 0), 6);              // nothing told: plain next
    CHECK_EQ((int)revAfter(5, 5), 6);
    CHECK_EQ((int)revAfter(5, 3), 6);              // told less: the desired wins
    // A local report at desired 5 was answered with rev 6 (the callback's
    // plan), and the store refused it: desired stays 5. The next web edit
    // must not be 6 — the node runs a 6 of its own and would say "up to date".
    const ReportPlan p = planReport(true, 5, 5, true);
    CHECK_EQ((int)reportAnswerRev(p, 5, true), 6);
    const uint16_t edit = revAfter(5, reportAnswerRev(p, 5, true));
    CHECK_EQ((int)edit, 7);
    CHECK(shouldSend(ST_PENDING, edit, 6));
    // An ACK for rev 9 against desired 5 (a restored node).
    CHECK_EQ((int)revAfter(5, 9), 10);
    // Across the wrap.
    CHECK_EQ((int)revAfter(0xFFFF, 1), 2);
    CHECK_EQ((int)revAfter(1, 0xFFFF), 2);
    CHECK_EQ((int)revAfter(0xFFFE, 0xFFFF), 1);
}

// ---------------------------------------------------------------------------
// Renames and file names
// ---------------------------------------------------------------------------

static void test_renamed_to_matches_desired_or_sent_name() {
    // Stored as "balcony"; renamed to "attic" and sent; renamed again to
    // "roof" before the node's next POST, which comes as "attic".
    CHECK(renamedTo("attic", "roof", "attic"));
    CHECK(renamedTo("roof", "roof", "attic"));
    CHECK(!renamedTo("garden", "roof", "attic"));
    CHECK(renamedTo("roof", "roof", nullptr));
    CHECK(!renamedTo("", "", ""));                 // an empty name matches nothing
    CHECK(!renamedTo("a/b", "a/b", "a/b"));
}

static void test_key_from_file_name() {
    bool espnow = false; char name[NODE_NAME_MAX + 1]; uint8_t id = 0;
    CHECK(keyFromFileName("w_balcony.json", espnow, name, id));
    CHECK(!espnow);
    CHECK_STREQ(name, "balcony");
    CHECK(keyFromFileName("e_3.json", espnow, name, id));
    CHECK(espnow);
    CHECK_EQ((int)id, 3);
    CHECK(keyFromFileName("w_abcdefghijklmnop.json", espnow, name, id));   // 16 letters
    CHECK_STREQ(name, "abcdefghijklmnop");
    // A leftover of an interrupted write, whole or cut to the buffer boot
    // lists names into: neither is a second file for the node.
    CHECK(!keyFromFileName("w_abcdefghijklmnop.json.tmp", espnow, name, id));
    CHECK(!keyFromFileName("w_balcony.json.tmp", espnow, name, id));
    CHECK(!keyFromFileName("handover.json", espnow, name, id));
    CHECK(!keyFromFileName("x_balcony.json", espnow, name, id));
    CHECK(!keyFromFileName("wxbalcony.json", espnow, name, id));
    CHECK(!keyFromFileName("e_0.json", espnow, name, id));
    CHECK(!keyFromFileName("w_.json", espnow, name, id));
    CHECK(!keyFromFileName(nullptr, espnow, name, id));
    // Every name a node can have fits the buffer it is listed into.
    char path[PATH_CAP];
    filePath(path, false, "abcdefghijklmnop", 0);
    CHECK(strlen(path) - strlen("/nodes/") < FILE_NAME_CAP);
}

// ===========================================================================
// DATA2 naming (§5)
// ===========================================================================

static NodeConfig probeNode() {
    NodeConfig c = espnowNode();
    SensorCfg a = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    a.count = 2;
    SensorCfg b = sensorDefaults(SensorType::Ds18b20, Hw::Esp32c3);
    b.pin = 5;
    copyStr(b.metric, sizeof(b.metric), "pool");
    addSensor(c, a);
    addSensor(c, b);
    return c;
}

static void test_probe_map_names_like_the_full_config() {
    const NodeConfig full = probeNode();
    ProbeMap m;
    probeMapFrom(full, m);
    CHECK_EQ((int)m.n, 2);
    CHECK_EQ((int)m.count[0], 2);
    CHECK_STREQ(m.metric[1], "pool");

    NodeConfig slim = configDefaults(Transport::EspNow, Hw::Esp32c3);
    probeMapToConfig(m, slim);
    for (uint8_t idx = 0; idx < 4; idx++) {
        char a[METRIC_NAME_CAP] = "", b[METRIC_NAME_CAP] = "";
        const char* ua = metricNameFor(&full, M_PROBE_TEMP, idx, a);
        const char* ub = metricNameFor(&slim, M_PROBE_TEMP, idx, b);
        CHECK((ua == nullptr) == (ub == nullptr));
        if (ua && ub) CHECK_STREQ(a, b);
    }
    // Non-probe ids do not depend on the map either way.
    char a[METRIC_NAME_CAP], b[METRIC_NAME_CAP];
    CHECK(metricNameFor(&full, M_TEMPERATURE, 0, a) != nullptr);
    CHECK(metricNameFor(&slim, M_TEMPERATURE, 0, b) != nullptr);
    CHECK_STREQ(a, b);
}

static void test_data2_named() {
    const NodeConfig c = probeNode();
    Data2Sample s;
    memset(&s, 0, sizeof(s));
    const Data2Value vals[] = {
        { M_TEMPERATURE, 0, 21.5f },
        { M_PROBE_TEMP, 1, 18.0f },
        { M_PROBE_TEMP, 2, 26.0f },
        { M_PROBE_TEMP, 3, 1.0f },               // no fourth probe: dropped
        { 99, 0, 1.0f },                         // unknown id: dropped
        { M_LUX, 0, NAN },                       // non-finite: dropped
        { M_LUX, 0, INFINITY },
        { M_BATTERY_VOLTAGE, 0, 3.91f },         // volts, as the catalogue says
    };
    s.n = (uint8_t)(sizeof(vals) / sizeof(vals[0]));
    for (uint8_t i = 0; i < s.n; i++) s.v[i] = vals[i];

    NamedValue out[EN_DATA2_MAX_VALUES];
    float battV = -1.0f;
    const int n = data2Named(&c, s, out, EN_DATA2_MAX_VALUES, battV);
    CHECK_EQ(n, 4);
    CHECK_STREQ(out[0].name, "temperature");
    CHECK_STREQ(out[0].unit, "C");
    CHECK_STREQ(out[1].name, "probe_temp_1");
    CHECK_STREQ(out[2].name, "pool");
    CHECK(out[2].value == 26.0f);
    CHECK_STREQ(out[3].name, "battery_voltage");
    CHECK_STREQ(out[3].unit, "V");
    CHECK(fabsf(battV - 3.91f) < 1e-6f);

    // Without a reported config: default names; and `max` is respected.
    battV = -1.0f;
    const int k = data2Named(nullptr, s, out, 2, battV);
    CHECK_EQ(k, 2);
    CHECK_STREQ(out[1].name, "probe_temp_1");
    CHECK(battV == -1.0f);                     // never reached: left alone
}

int main() {
    RUN(test_status_names_round_trip);
    RUN(test_next_rev_skips_zero);
    RUN(test_status_after_applied);
    RUN(test_should_send_only_pending_and_behind);
    RUN(test_revs_compare_across_the_wrap);
    RUN(test_valid_name);
    RUN(test_parse_key);
    RUN(test_format_key_and_path);
    RUN(test_plan_report_no_config_held);
    RUN(test_plan_report_local_edit_wins);
    RUN(test_plan_report_status_only);
    RUN(test_report_answer_rev);
    RUN(test_ingest_reply);
    RUN(test_secret_bits);
    RUN(test_report_secrets_known);
    RUN(test_secrets_for_node_sends_only_dirty);
    RUN(test_secrets_for_get_reports_knowledge);
    RUN(test_adopt_table_identity);
    RUN(test_handover_classify);
    RUN(test_handover_auto_switch);
    RUN(test_handover_apply_next);
    RUN(test_handover_keeps_next_over_a_local_edit);
    RUN(test_rev_after_moves_past_what_the_node_was_told);
    RUN(test_renamed_to_matches_desired_or_sent_name);
    RUN(test_key_from_file_name);
    RUN(test_probe_map_names_like_the_full_config);
    RUN(test_data2_named);
    return SUMMARY();
}
