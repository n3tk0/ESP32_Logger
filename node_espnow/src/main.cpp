// ============================================================================
// node_espnow/src/main.cpp — the battery node
//
// One wake, start to finish:
//
//     boot → read battery → read sensors → send DATA2 → wait ≤ ack window
//          → [config exchange, only when there is one]
//          → [firmware download, only when there is one] → sleep
//
// setup() does all of it and never returns to loop(), because deep sleep is a
// reset: there is no main loop on a device that is awake for a third of a
// second. loop() runs only in MAINS MODE (config `sleep: false`), where the
// node stays awake and reports every interval_s from a delay loop — the mode
// that allows an SDS011 (its fan needs ~30 s) or a pulse counter (it counts
// in an interrupt), neither of which survives deep sleep.
//
// WHAT SURVIVES A SLEEP, AND WHERE
// --------------------------------
// RTC memory  the sequence number, the failure counters, the readings that
//             could not be delivered (Backlog.h), the config exchange's
//             bookkeeping and how far a firmware download got (FwFetch.cpp).
//             Survives deep sleep, a software restart and a
//             watchdog reset, costs nothing to write, and is gone on a power
//             cut — which is correct for all of it.
// NVS         the link — the collector's MAC, the channel, the node id, the
//             access point to look for ("espnow-node") — and the config
//             document and page key ("cfg", ConfigStore.h), and a new
//             firmware's trial ("nodefw", FwTrial.h). Written only when
//             something actually changed, because flash wears out.
//
// Nothing is written to flash on an ordinary wake. A node that stored its
// sequence number in NVS would do a minute-by-minute write for a year.
// ============================================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "Backlog.h"
#include "CfgApply.h"
#include "CfgFetch.h"
#include "ConfigStore.h"
#include "FwFetch.h"
#include "FwTrial.h"
#include "Link.h"
#include "Portal.h"
#include "Rescan.h"
#include "node_common/NodeSensors.h"
#include "node_config.h"
#include "src/espnow/EspNowProto.h"
#include "src/nodecfg/FwImage.h"
#include "src/nodecfg/NodeConfigJson.h"

using namespace nodecfg;

/// What makes this .bin an ESP-NOW node image to the collector, to the page's
/// /update and to another node's download (docs/NODE_OTA.md §1.1). Printed at
/// a cold boot: --gc-sections drops an array nothing references, and an image
/// without its marker is refused everywhere.
static const char FW_MARKER[] = NODEFW_MARKER_TEXT("espnow-c3", NODE_FW_VERSION);

// ---------------------------------------------------------------------------
// State that outlives the sleep
// ---------------------------------------------------------------------------

/// Distinguishes a deep-sleep wake from a cold boot. RTC memory is preserved
/// across the former and undefined after the latter, so without a marker the
/// first frame after a power-on carries whatever was in the SRAM. "NOD2": the
/// layout changed with the variable-length backlog, and a wake must never
/// read an old layout as the new one.
static const uint32_t RTC_MAGIC = 0x4E4F4432;   // "NOD2"

// RTC_NOINIT_ATTR, not RTC_DATA_ATTR. Both survive deep sleep, but the
// startup code reloads RTC_DATA_ATTR from the image on every other reset —
// so esp_restart() (the bench build's "sleep") and a watchdog reset threw
// the backlog away and started the sequence over. NOINIT is left alone by
// every reset and is garbage after a power-on, which is what the checks in
// setup() are for: a cold start by reset reason, the magic, the image that
// wrote it (s_rtcImage) and enbl::valid().
RTC_NOINIT_ATTR static uint32_t s_rtcMagic;
/// Four bytes of the running image's ELF SHA-256. A new firmware may lay
/// these variables out differently, and it is flashed with a reset that need
/// not count as a power-on, so the magic alone could read an old layout as
/// the new one.
RTC_NOINIT_ATTR static uint32_t s_rtcImage;
RTC_NOINIT_ATTR static uint16_t s_seq;
RTC_NOINIT_ATTR static uint8_t  s_failStreak;     ///< consecutive unanswered wakes
RTC_NOINIT_ATTR static uint16_t s_wakesSinceScan; ///< see the rescan gate
RTC_NOINIT_ATTR static uint32_t s_wakeCount;
/// EN_FLAG_FIRST_BOOT goes on every frame until one is ACKed, not only on the
/// first: seq restarted at 0, and if that first frame is lost the collector's
/// replay guard would otherwise drop the next ones as stale.
RTC_NOINIT_ATTR static bool     s_firstBootPending;

/// Readings the node could not deliver — see Backlog.h. A node that cannot
/// reach its collector for twenty minutes should not silently lose twenty
/// minutes of weather; it hands them over when the link comes back, each
/// with the time it was actually taken.
RTC_NOINIT_ATTR static enbl::Pool s_backlog;

/// The config exchange (docs/NODE_CONFIG.md §5).
RTC_NOINIT_ATTR static bool           s_reportDue;    ///< CFG_REPORT still owed
RTC_NOINIT_ATTR static encfg::Backoff s_fetchBackoff;
RTC_NOINIT_ATTR static encfg::Backoff s_reportBackoff;
RTC_NOINIT_ATTR static uint16_t       s_rejectedRev;  ///< last rev refused, 0 = none
RTC_NOINIT_ATTR static char           s_rejField[EN_CFG_FIELD_LEN];
RTC_NOINIT_ATTR static char           s_rejReason[EN_CFG_REASON_LEN];

/// This image's id: four bytes of its ELF SHA-256, which every build changes.
/// Which firmware wrote the RTC state, and the number the collector compares
/// with an image it holds (docs/NODE_OTA.md §1.3: nodefw::c3ImageId() reads
/// the same bytes at 0xB0 of the .bin).
static uint32_t runningImageId() {
    const esp_app_desc_t* d = esp_ota_get_app_description();
    uint32_t id = 0;
    if (d) memcpy(&id, d->app_elf_sha256, sizeof(id));
    return id;
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

/// Adopt a wall clock handed to us by the collector.
static void adoptClock(uint32_t epoch) {
    if (epoch < 1000000000u) return;
    struct timeval tv = {};
    tv.tv_sec = (time_t)epoch;
    settimeofday(&tv, nullptr);
}

/// Wall clock, or 0 when there isn't one.
///
/// time() on a node that has never been told the time returns seconds since
/// boot, which is a small number and not an epoch. Storing THAT as a sample's
/// timestamp and later subtracting it from a real epoch produced an age of
/// fifty-five years, clamped to the field's 65535 seconds — so a reading taken
/// a minute ago shipped claiming to be eighteen hours old.
///
/// One function, so the guard cannot be applied at three call sites and
/// forgotten at a fourth. It was.
static inline uint32_t usableEpoch() {
    const uint32_t t = (uint32_t)time(nullptr);
    return (t >= 1000000000u) ? t : 0;
}

// ---------------------------------------------------------------------------
// Persistent state
// ---------------------------------------------------------------------------

static Preferences s_prefs;
static NodeLink    s_link;
static NodeConfig  s_cfg;        ///< the applied config (ConfigStore.h)
static bool        s_mains = false;

static void loadLink() {
    memset(&s_link, 0, sizeof(s_link));
    s_prefs.begin("espnow-node", true);
    s_link.nodeId    = s_prefs.getUChar("id", 0);
    s_link.channel   = s_prefs.getUChar("ch", 0);
    s_prefs.getBytes("mac", s_link.collector, 6);
    s_prefs.getBytes("bssid", s_link.bssid, 6);
    s_prefs.getString("ssid", s_link.ssid, sizeof(s_link.ssid));
    s_prefs.end();
}

static void saveLink() {
    s_prefs.begin("espnow-node", false);
    s_prefs.putUChar("id", s_link.nodeId);
    s_prefs.putUChar("ch", s_link.channel);
    s_prefs.putBytes("mac", s_link.collector, 6);
    s_prefs.putBytes("bssid", s_link.bssid, 6);
    s_prefs.putString("ssid", s_link.ssid);
    s_prefs.end();
}

/// Store the channel only when it actually changed.
///
/// Called after every successful report, because the ACK carries the
/// collector's channel — and writing it unconditionally would be a flash
/// write a minute, which is exactly the mistake the RTC-memory split above
/// exists to avoid.
static void rememberChannel(uint8_t ch) {
    if (!ch || ch == s_link.channel) return;
    s_link.channel = ch;
    s_prefs.begin("espnow-node", false);
    s_prefs.putUChar("ch", ch);
    s_prefs.end();
    Serial.printf("[node] channel is now %u\n", ch);
}

/// The collector's legacy interval push — AckMsg.intervalS, and the WELCOME's
/// interval at pairing. Still honoured (a collector that only knows the old
/// way must keep working), and written into the config document so the two
/// cannot disagree. Two exceptions:
///   * while `local` is true: an edit made on the node's own page wins until
///     the collector has adopted it (principle 4);
///   * the caller skips it on an ACK that also flags a pending config — the
///     config is the authoritative source, and it is about to be fetched.
static void applyIntervalPush(uint16_t iv) {
    if (!iv || s_cfg.local || iv == s_cfg.interval_s) return;
    if (iv < INTERVAL_MIN_S) iv = INTERVAL_MIN_S;
    if (iv == s_cfg.interval_s) return;
    s_cfg.interval_s = iv;
    cfgStoreSave(s_cfg);
    Serial.printf("[node] collector asked for %us\n", (unsigned)iv);
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

/// Cell voltage in millivolts, or 0 when it cannot be measured.
///
/// analogReadMilliVolts() and not analogRead(): the ESP32-C3's ADC is neither
/// linear nor consistent part to part, and the raw counts are out by up to
/// 10 %. That function applies the calibration burned into the chip's eFuses
/// at the factory, which is the difference between a remaining-life estimate
/// that means something and one that is confidently wrong. batt.trim is there
/// for what the resistors get wrong on top of that.
static uint16_t readBatteryMv(const BattCfg& b) {
    analogSetPinAttenuation(b.pin, ADC_11db);

    uint32_t sum = 0;
    for (int i = 0; i < NODE_BATT_SAMPLES; i++)
        sum += analogReadMilliVolts(b.pin);

    const float atPin = (float)sum / (float)NODE_BATT_SAMPLES;
    const float mv    = atPin * b.divider * b.trim;

    // Nothing plausible is below a volt. A node with no divider fitted reads
    // near zero, and reporting that as a battery voltage would put a node at
    // 0 % on the collector's dashboard for ever.
    if (mv < 1000.0f || mv > 5000.0f) return 0;
    return (uint16_t)(mv + 0.5f);
}

/// For the setup page's /api/status.
static float battVolts() { return (float)readBatteryMv(s_cfg.batt) / 1000.0f; }

// ---------------------------------------------------------------------------
// One reading
// ---------------------------------------------------------------------------

/// How a sensor's measurement is waited out (nodeSensorsSetWait): a DS18B20
/// conversion and a one-shot BH1750 (180 ms), which run together. At 12 bits
/// the probe needs up to 750 ms, and the reading has to come from THIS wake —
/// reading straight away returns the previous conversion, 85 °C after a
/// power-on. Spent in delay() that is ~0.76 s at the C3's radio-off idle,
/// about 20 mA: ~6 mAh a day at one-minute intervals, more than half again
/// the node's whole budget (docs/ESPNOW_NODE.md §7, §9). The probe converts
/// from its own 3V3 supply and the bus idles high on its 4.7 kΩ pull-up, so
/// nothing needs the CPU meanwhile: light sleep, ~0.2 mA with the board. The
/// same holds for a BH1750, which measures on its own and powers itself down.
///
/// Only on a battery. A mains node may be counting pulses in an interrupt or
/// receiving an SDS011 on a UART, neither of which runs in light sleep (the
/// validator refuses both with sleep on). The bench build keeps its USB
/// console, which light sleep would drop. The radio is always off here:
/// collectLive() runs before linkBegin().
///
/// The wake-up timer runs on the calibrated RTC slow clock; the 1/32 extra
/// (24 ms at 12 bits, 6 ms for a BH1750, at light-sleep current) covers its
/// error, so neither is read mid-measurement.
static void waitConversion(uint32_t ms) {
#ifndef NODE_NO_DEEP_SLEEP
    if (s_cfg.sleep) {
        const uint64_t us = (uint64_t)ms * 1000ULL + (uint64_t)ms * 1000ULL / 32;
        if (esp_sleep_enable_timer_wakeup(us) == ESP_OK && esp_light_sleep_start() == ESP_OK)
            return;
    }
#endif
    delay(ms);
}

/// Everything this wake measured, as DATA2 values: the sensor layer's
/// readings in listMetrics() order, then battery_voltage (metric 14, in
/// VOLTS — the catalogue's unit) when there is a divider to read.
///
/// A metric that is not answering is simply absent, never a zero: a BMP280
/// has no humidity sensor, and 0 %RH on the wire would be a reading the
/// collector could not tell from a real one.
static uint8_t collectLive(Data2Value* out, uint16_t vbatMv) {
    NodeReading r[NODE_MAX_READINGS];
    const int k = nodeSensorsRead(s_cfg, r, NODE_MAX_READINGS);
    uint8_t n = 0;
    for (int i = 0; i < k && n < EN_DATA2_MAX_VALUES - 1; i++) {
        if (!isfinite(r[i].value)) continue;
        out[n].metric = r[i].metricId;
        out[n].index  = r[i].index;
        out[n].value  = r[i].value;
        n++;
    }
    if (vbatMv) {
        out[n].metric = M_BATTERY_VOLTAGE;
        out[n].index  = 0;
        out[n].value  = (float)vbatMv / 1000.0f;
        n++;
    }
    return n;
}

static void bufferLive(const Data2Value* v, uint8_t n) {
    enbl::push(s_backlog, usableEpoch(), v, n);
}

// ---------------------------------------------------------------------------
// The config exchange — docs/NODE_CONFIG.md §5
// ---------------------------------------------------------------------------

static uint16_t cfgReplyWindow() {
    return s_cfg.link.ack_window_ms > NODE_CFG_REPLY_MS ? s_cfg.link.ack_window_ms
                                                       : (uint16_t)NODE_CFG_REPLY_MS;
}

static void sendCfgAck(uint16_t rev, uint8_t status, const char* field, const char* reason) {
    CfgAckMsg m;
    espnowFillCfgAck(m, s_link.nodeId, rev, status, field, reason);
    const bool ok = linkSendFrame(s_link, &m, sizeof(m));
    Serial.printf("[cfg] CFG_ACK rev %u %s%s%s%s -> %s\n", (unsigned)rev,
                  status == EN_CFG_OK ? "ok" : "rejected",
                  status == EN_CFG_OK ? "" : " ", field ? field : "",
                  status == EN_CFG_OK ? "" : ": ", ok ? "sent" : "not delivered");
    (void)reason;
}

static void rejectCfg(uint16_t rev, const char* field, const char* reason) {
    s_rejectedRev = rev;
    copyStr(s_rejField, sizeof(s_rejField), field ? field : "");
    copyStr(s_rejReason, sizeof(s_rejReason), reason ? reason : "");
    Serial.printf("[cfg] rev %u refused: %s: %s\n", (unsigned)rev, s_rejField, s_rejReason);
    sendCfgAck(rev, EN_CFG_REJECTED, s_rejField, s_rejReason);
}

/// A whole document arrived: decode, validate (CfgApply.h), save, apply,
/// answer. On any failure the running config is untouched (principle 3).
///
/// False only when the flash write failed. That is not the document's fault,
/// so the rev is NOT remembered as refused and no CFG_ACK goes out: the
/// collector keeps flagging it and the fetch is tried again after the
/// backoff, instead of a good config being refused until the next boot.
static bool applyConfigDoc(const EnCfgAssembler& a) {
    static NodeConfig next;           // ~1 KB: off the loop task's stack
    Issue why;
    if (!encfg::acceptFromCollector(s_cfg, a.doc, a.total, a.rev, next, why)) {
        rejectCfg(a.rev, why.field, why.reason);
        return true;
    }
    if (!cfgStoreSave(next)) {
        Serial.printf("[cfg] rev %u could not be written to flash — will retry\n",
                      (unsigned)a.rev);
        return false;
    }

    const bool wasMains = !s_cfg.sleep;
    s_cfg = next;
    Serial.printf("[cfg] applied rev %u\n", (unsigned)s_cfg.rev);
    sendCfgAck(a.rev, EN_CFG_OK, "", "");

    // Applying: everything but the sensors is read fresh where it is used
    // (interval, ack window, rescan limits, battery divider). The sensors are
    // brought up at every wake in sleep mode, so only a node that stays up
    // has to redo them now; a switch between sleep and mains takes effect at
    // the end of this wake (see loop() / finishWake()).
    if (wasMains && !s_cfg.sleep) nodeSensorsBegin(s_cfg);
    return true;
}

/// Pull the desired config: CFG_GET from offset 0, one slice per request,
/// bounded by NODE_CFG_BUDGET_MS. CfgFetch.h has the reasoning and the cost.
static void fetchConfig() {
    static encfg::Fetch f;             // holds the 1 KB assembler
    encfg::begin(f, s_link.nodeId, s_cfg.rev, s_rejectedRev, millis(), NODE_CFG_BUDGET_MS,
                 NODE_CFG_MAX_MISSES);
    const uint32_t t0 = millis();
    CfgGetMsg   req;
    CfgChunkMsg rep;
    for (;;) {
        if (!encfg::wantRequest(f, millis(), req)) {
            Serial.printf("[cfg] fetch gave up at offset %u after %lu ms\n",
                          (unsigned)f.offset, (unsigned long)(millis() - t0));
            encfg::failed(s_fetchBackoff);
            return;
        }
        uint32_t w = encfg::remainingMs(f, millis());
        if (w > cfgReplyWindow()) w = cfgReplyWindow();
        const bool got = linkExchangeCfg(s_link, &req, sizeof(req), (uint16_t)w, rep);

        switch (encfg::onReply(f, got ? &rep : nullptr)) {
            case encfg::Step::Request:
                continue;
            case encfg::Step::Complete:
                Serial.printf("[cfg] rev %u fetched (%u bytes) in %lu ms\n", (unsigned)f.a.rev,
                              (unsigned)f.a.total, (unsigned long)(millis() - t0));
                if (applyConfigDoc(f.a)) encfg::succeeded(s_fetchBackoff);
                else                     encfg::failed(s_fetchBackoff);
                return;
            case encfg::Step::UpToDate:
                // The collector is offering what we already run: our CFG_ACK
                // for it was lost. Say it again so it stops flagging.
                if (f.rev == s_cfg.rev) sendCfgAck(s_cfg.rev, EN_CFG_OK, "", "");
                encfg::succeeded(s_fetchBackoff);
                return;
            case encfg::Step::KnownRejected:
                // Already refused this exact rev: repeat the refusal instead
                // of downloading it again, and back off in case the collector
                // keeps flagging it anyway.
                sendCfgAck(f.rev, EN_CFG_REJECTED, s_rejField, s_rejReason);
                encfg::failed(s_fetchBackoff);
                return;
        }
    }
}

/// Send the running config as CFG_REPORT slices, and wait after the last for
/// the collector's answer (a CFG with total == 0). For a local edit that says
/// "your local config is now rev N": adopt N and clear `local`. For the report
/// owed since boot it only says the report landed. Either way the report stays
/// owed until the answer comes, and is sent again on a later wake under
/// s_reportBackoff (CfgFetch.h, "Reporting").
static void reportConfig() {
    char doc[EN_CFG_MAX_TOTAL + 1];
    const size_t n = encodeConfigTo(s_cfg, doc, sizeof(doc), 0);
    if (!n || n > EN_CFG_MAX_TOTAL) {
        Serial.println("[cfg] config does not fit a report");
        s_reportDue = false;
        return;
    }
    CfgChunkMsg m;
    for (uint16_t off = 0; off < n; off = (uint16_t)(off + m.len)) {
        const int flen = espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, s_link.nodeId, s_cfg.rev,
                                            doc, (uint16_t)n, off);
        if (flen < 0) break;
        if ((uint32_t)off + m.len < n) {
            if (!linkSendFrame(s_link, &m, flen)) break;
            continue;
        }
        CfgChunkMsg rep;
        const bool got = linkExchangeCfg(s_link, &m, flen, cfgReplyWindow(), rep);
        if (!encfg::reportAnswered(got ? &rep : nullptr, s_link.nodeId, s_cfg.local)) break;
        if (s_cfg.local) {
            s_cfg.rev   = rep.rev;
            s_cfg.local = false;
            cfgStoreSave(s_cfg);
            Serial.printf("[cfg] collector adopted the local config as rev %u\n",
                          (unsigned)rep.rev);
        }
        s_reportDue = false;
        encfg::succeeded(s_reportBackoff);
        return;
    }
    encfg::failed(s_reportBackoff);
    Serial.println("[cfg] config reported, no answer yet");
}

// ---------------------------------------------------------------------------
// §4.6: the collector's network moved while we slept
// ---------------------------------------------------------------------------

/// A collector answered a (re-)pairing: it may be a different one, or one
/// that lost what it knew of us, so the config report owed after a boot is
/// owed again — and now, not after whatever backoff a silent collector built.
static void reportAfterPairing() {
    s_reportDue = true;
    encfg::succeeded(s_reportBackoff);
}

/// Promote link.next_ssid to the stored network, and keep the old one as
/// next, so a handover the collector cancels is survivable the same way.
/// `local` is set so the collector learns the swap on the next contact.
///
/// Only once the collector is confirmed there (§4.6 "on success"): a signed
/// DISCOVER on that one channel, which it answers from its node table without
/// a pairing window. A network on the air is not a collector on it — one
/// that is restarting would otherwise send the node to the wrong channel
/// until the next scan an hour later. Unconfirmed, nothing is stored and the
/// next eligible scan looks again.
static void adoptNextNetwork(const ScanResult& sr) {
    NodeLink fresh = s_link;
    fresh.nodeId = 0;
    uint32_t e = 0;
    if (!linkPair(fresh, &e, sr.nextCh)) {
        Serial.printf("[node] \"%s\" is on channel %u but no collector answered there\n",
                      s_cfg.link.next_ssid, (unsigned)sr.nextCh);
        return;
    }
    adoptClock(e);

    char old[sizeof(s_link.ssid)];
    copyStr(old, sizeof(old), s_link.ssid);
    s_link = fresh;
    // The WELCOME names the collector's network; an empty one (a collector
    // on its own AP) falls back to what the scan heard.
    if (!s_link.ssid[0]) {
        copyStr(s_link.ssid, sizeof(s_link.ssid), s_cfg.link.next_ssid);
        memcpy(s_link.bssid, sr.nextBssid, 6);
    }
    saveLink();
    s_failStreak = 0;
    reportAfterPairing();

    copyStr(s_cfg.link.next_ssid, sizeof(s_cfg.link.next_ssid), old);
    s_cfg.local = true;
    cfgStoreSave(s_cfg);
    Serial.printf("[node] followed the collector to \"%s\" on channel %u\n", s_link.ssid,
                  (unsigned)s_link.channel);
}

/// Nobody answered for long enough: go looking for where the network moved.
///
/// Rate limited, and the rate limit is measured from the last scan and not
/// from a clock: a channel change is recovered at THIS wake, not on the hour.
/// The ceiling exists for the other case — a collector simply switched off —
/// where scanning every minute would cost more radio than reporting does.
/// Counted in WAKES, not in seconds off the clock. The clock is the one thing
/// here that can jump: settimeofday() moves it from a few hundred to 1.75
/// billion the first time a collector answers, and a rate limit measured
/// against it is either bypassed or stuck depending on which side of that
/// jump the two readings fell. Wakes are a count this node cannot get wrong.
static void maybeRescan() {
    if (!enrescan::due(s_failStreak, s_wakesSinceScan, s_cfg.link.rescan_fails,
                       s_cfg.link.rescan_min_s, s_cfg.interval_s))
        return;
    s_wakesSinceScan = 0;

    const ScanResult sr = linkFindChannel(s_link, s_cfg.link.next_ssid);
    switch (enrescan::decide(sr.storedCh, sr.nextCh, s_link.channel)) {
        case enrescan::Action::MoveChannel:
            Serial.printf("[node] access point moved to channel %u\n", sr.storedCh);
            rememberChannel(sr.storedCh);
            return;
        case enrescan::Action::AdoptNext:
            adoptNextNetwork(sr);
            return;
        case enrescan::Action::None:
            return;
        case enrescan::Action::Sweep:
            break;
    }

    // The access point is not on the air at all, or it is where it always was
    // and the collector still does not answer — reflashed (it can no longer
    // decrypt us, so it can never say so) or moved to a network we were never
    // told about. The collector answers a signed DISCOVER from a node in its
    // table even without a pairing window (§4.6), so sweeping is the way back
    // from both, and cheap enough once an hour.
    Serial.println("[node] no collector on the stored channel — sweeping for it");
    NodeLink fresh = s_link;
    fresh.nodeId = 0;
    uint32_t e = 0;
    if (linkPair(fresh, &e)) {
        adoptClock(e);
        s_link = fresh;
        saveLink();
        s_failStreak = 0;
        applyIntervalPush(s_link.intervalS);
        reportAfterPairing();
        Serial.printf("[node] re-paired as node %u\n", s_link.nodeId);
    }
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

static void sleepNow(uint16_t seconds) {
    // NEVER ZERO. This is a battery device whose entire power budget is the
    // ratio of this number to the second or so a wake costs, so a 0 here is
    // not a short interval — it is a node that never sleeps again and a cell
    // flat within a day. The validator refuses interval_s < 10 and the store
    // refuses a document that does not validate, but this is the one number
    // that must not be wrong, so it defends itself as well.
    if (seconds < INTERVAL_MIN_S) {
        seconds = NODE_INTERVAL_S >= INTERVAL_MIN_S ? NODE_INTERVAL_S : 60;
        Serial.printf("[node] interval was below the minimum — sleeping %us instead\n",
                      seconds);
    }
    Serial.printf("[node] wake %u done in %lu ms, sleeping %us\n",
                  (unsigned)s_wakeCount, (unsigned long)millis(), seconds);
    Serial.flush();
    nodeSensorsEnd();

#ifdef NODE_NO_DEEP_SLEEP
    delay((uint32_t)seconds * 1000u);
    esp_restart();     // a reset, so each pass starts where a real wake would
#else
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
#endif
}

// ---------------------------------------------------------------------------
// One wake
// ---------------------------------------------------------------------------

static void wake() {
    s_wakeCount++;
    if (s_wakesSinceScan < 0xFFFF) s_wakesSinceScan++;

    // Measured before the radio comes up, while the board is drawing least.
    // Under the transmit burst the cell reads several tens of millivolts lower
    // and the reading would be about the antenna rather than the battery.
    const uint16_t vbat = readBatteryMv(s_cfg.batt);
    Data2Value live[EN_DATA2_MAX_VALUES];
    const uint8_t liveN = collectLive(live, vbat);

    if (!linkBegin(s_link)) {
        Serial.println("[node] radio failed to start");
        bufferLive(live, liveN);
        fwTrialAfterWake(false);
        return;
    }

    // ── Never provisioned: find a collector ─────────────────────────────────
    if (s_link.nodeId == 0) {
        Serial.println("[node] no collector known — sweeping for one");
        uint32_t pairedEpoch = 0;
        if (linkPair(s_link, &pairedEpoch)) {
            adoptClock(pairedEpoch);
            saveLink();
            applyIntervalPush(s_link.intervalS);
            reportAfterPairing();
            Serial.printf("[node] paired as node %u on channel %u\n",
                          s_link.nodeId, s_link.channel);
        } else {
            // The collector only answers while a pairing window is open, and
            // that window is opened by a human. Sleeping a full interval
            // between attempts is right: sweeping continuously would empty the
            // cell before anybody got to the collector.
            Serial.println("[node] nobody answered — is the collector's pairing window open?");
            linkEnd();
            bufferLive(live, liveN);
            fwTrialAfterWake(false);
            return;
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    const uint32_t now = usableEpoch();
    uint8_t frame[ESPNOW_MAX_FRAME];
    uint8_t taken = 0;
    const int len = enbl::buildFrame(s_backlog, frame, sizeof(frame), s_link.nodeId, s_seq,
                                     s_firstBootPending ? EN_FLAG_FIRST_BOOT : 0, now,
                                     live, liveN, taken);

    const LinkResult r = linkSendData(s_link, frame, len, s_seq, s_cfg.link.ack_window_ms);
    Serial.printf("[node] sent %u+1 sample(s), %d bytes, seq=%u -> sent=%d ack=%d waited=%lums\n",
                  (unsigned)taken, len, (unsigned)s_seq, (int)r.sent, (int)r.acked,
                  (unsigned long)r.waitedMs);
    s_seq++;                       // the frame went out; never reuse its number

    // A new image on trial is confirmed by the first ACK — before anything
    // else this wake does — or counts one more unanswered wake (FwTrial.h).
    // Rolling back restarts here.
    fwTrialAfterWake(r.acked);

    if (r.acked && r.rediscover) {
        // The collector answered but does not know us, so it dropped the
        // frame: nothing in it was delivered. Keep the backlog and this
        // reading. Pairing again is a ~1.6 s sweep, so it goes through the
        // rescan's rate limit — a collector whose table is full, or one that
        // keeps forgetting us, must not cost a sweep every wake.
        s_failStreak = 0;
        bufferLive(live, liveN);
        adoptClock(r.epoch);
        rememberChannel(r.channel);
        if (enrescan::spaced(s_wakesSinceScan, s_cfg.link.rescan_min_s, s_cfg.interval_s)) {
            s_wakesSinceScan = 0;
            Serial.println("[node] collector no longer knows us — pairing again");
            uint32_t e = 0;
            if (linkPair(s_link, &e)) {
                adoptClock(e);
                saveLink();
                applyIntervalPush(s_link.intervalS);
                reportAfterPairing();
                Serial.printf("[node] re-paired as node %u\n", s_link.nodeId);
            }
        } else {
            Serial.println("[node] collector no longer knows us — pairing again later");
        }
    } else if (r.acked) {
        s_failStreak       = 0;
        s_firstBootPending = false;
        enbl::dropOldest(s_backlog, taken);   // delivered; exactly those

        // The only clock this node has. No NTP — it never associates — and no
        // 32 kHz crystal on this board, so deep sleep is timed by an RC
        // oscillator that drifts by percent. Resynchronising every wake is
        // what keeps a buffered burst's timestamps worth anything.
        adoptClock(r.epoch);
        if (!r.cfgPending) applyIntervalPush(r.intervalS);
        rememberChannel(r.channel);

        // Local edits win; the report owed since boot goes before a fetch
        // but cannot hold one up (CfgFetch.h, choose()).
        switch (encfg::choose(s_cfg.local, s_reportDue, r.cfgPending, s_reportBackoff,
                              s_fetchBackoff)) {
            case encfg::Exchange::Report: reportConfig(); break;
            case encfg::Exchange::Fetch:  fetchConfig();  break;
            case encfg::Exchange::None:   break;
        }

        // Firmware second: a config is small and may be what makes the node
        // reachable at all, an image is a minute of radio (FwFetch.h). A
        // staged image restarts from inside; otherwise this returns.
        if (r.fwPending)
            fwOnPending(s_link, runningImageId(), vbat, s_mains,
                        s_cfg.link.ack_window_ms > NODE_FW_REPLY_MS ? s_cfg.link.ack_window_ms
                                                                    : (uint16_t)NODE_FW_REPLY_MS);
    } else {
        if (s_failStreak < 255) s_failStreak++;
        bufferLive(live, liveN);
        maybeRescan();
    }

    linkEnd();
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

/// A power-on or the reset button, as opposed to a deep-sleep wake or a
/// software restart (the portal's, or the bench build's).
static bool isColdStart() {
    const esp_reset_reason_t why = esp_reset_reason();
    return why == ESP_RST_POWERON || why == ESP_RST_EXT;
}

/// Is `pin` wired to something in this config? The mains loop does not poll
/// the portal button over a sensor that is using the same GPIO.
static bool pinInUse(const NodeConfig& c, uint8_t pin) {
    if (c.batt.pin == pin) return true;
    for (uint8_t i = 0; i < c.sensor_count; i++) {
        const SensorCfg& s = c.sensors[i];
        if (sensorIsI2c(s.type) && (c.i2c.sda == pin || c.i2c.scl == pin)) return true;
        if ((s.type == SensorType::Ds18b20 || s.type == SensorType::Pulse) && s.pin == pin)
            return true;
        if (s.type == SensorType::Sds011 && (s.rx == pin || s.tx == pin)) return true;
    }
    return false;
}

void setup() {
    Serial.begin(115200);
#ifdef NODE_NO_DEEP_SLEEP
    delay(200);
    Serial.println("[node] BENCH BUILD — no deep sleep. Do not run this on a battery.");
#endif

    // Before anything that could crash: an image on trial that crash-loops
    // must still get as far as counting this start (FwTrial.h).
    fwTrialBoot();

    // A panic or a brownout starts over too: the state that survived may be
    // what crashed it, and a kept state would crash it again on every boot.
    const esp_reset_reason_t why = esp_reset_reason();
    const uint32_t image    = runningImageId();
    const bool     coldBoot = isColdStart() || why == ESP_RST_PANIC ||
                              why == ESP_RST_BROWNOUT || s_rtcMagic != RTC_MAGIC ||
                              s_rtcImage != image || !enbl::valid(s_backlog);
    if (coldBoot) {
        s_rtcMagic         = RTC_MAGIC;
        s_rtcImage         = image;
        s_seq              = 0;
        s_failStreak       = 0;
        s_wakesSinceScan   = 0xFFFF;   // scan on the first eligible failure
        s_wakeCount        = 0;
        s_firstBootPending = true;
        s_reportDue        = true;     // §5: report on the first wake after boot
        s_fetchBackoff     = encfg::Backoff{0, 0};
        s_reportBackoff    = encfg::Backoff{0, 0};
        s_rejectedRev      = 0;
        s_rejField[0]      = '\0';
        s_rejReason[0]     = '\0';
        enbl::clear(s_backlog);
        fwColdBoot();
        Serial.printf("[node] %s, image %08lx\n", FW_MARKER, (unsigned long)image);
    }

    loadLink();
    cfgStoreLoad(s_cfg);
    linkSetKey(cfgActiveKey(s_cfg));

    // ── The setup page ──────────────────────────────────────────────────────
    // Asked for with the BOOT button, or opened by itself on a power-on when
    // the node can do nothing else: never paired, and no key but the
    // placeholder. After it times out it restarts, which is not a cold start,
    // so a node nobody configures carries on with what it was built with
    // instead of holding an AP up until the cell is flat.
    const bool cold = isColdStart();
    bool portal = portalRequested(cold);
    if (!portal && cold && s_link.nodeId == 0 && cfgKeyIsPlaceholder(s_cfg)) {
        Serial.println("[node] not paired and no key set — opening the setup page");
        portal = true;
    }
    if (portal) portalRun(s_cfg, s_link, battVolts);

    s_mains = !s_cfg.sleep;
    nodeSensorsSetWait(waitConversion);   // decides per call, from s_cfg.sleep
    const int up = nodeSensorsBegin(s_cfg);
    if (coldBoot)
        Serial.printf("[node] %s, %d sensor(s) up: %s\n", s_mains ? "mains mode" : "battery mode",
                      up, nodeSensorsDescribe());

    if (s_mains) return;           // loop() takes over

    wake();
    sleepNow(s_cfg.interval_s);
}

/// Mains mode only (`sleep: false`): report every interval_s and stay up in
/// between, so the sensors that cannot sleep keep running. The radio still
/// comes up per report and goes down after it — nothing listens between
/// reports, because the node always starts the conversation.
void loop() {
    const uint32_t t0 = millis();
    wake();

    // A config from the collector may have turned sleep back on.
    if (s_cfg.sleep) {
        Serial.println("[node] config switched this node to battery mode");
        sleepNow(s_cfg.interval_s);
    }

    const bool pollButton = !pinInUse(s_cfg, NODE_PORTAL_PIN);
    const uint32_t periodMs = (uint32_t)(s_cfg.interval_s >= INTERVAL_MIN_S
                                             ? s_cfg.interval_s : NODE_INTERVAL_S) * 1000u;
    while (millis() - t0 < periodMs) {
        // Held for a second: the setup page, without a trip to RESET.
        if (pollButton && portalButtonHeld()) {
            const uint32_t held = millis();
            while (portalButtonHeld() && millis() - held < 1000) delay(20);
            if (millis() - held >= 1000) {
                nodeSensorsEnd();
                portalRun(s_cfg, s_link, battVolts);
            }
        }
        delay(50);
    }
}
