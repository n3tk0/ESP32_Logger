// ============================================================================
// node_espnow/src/main.cpp — the battery node
//
// One wake, start to finish:
//
//     boot → read battery → read sensor → send → wait ≤30 ms → sleep
//
// setup() does all of it and never returns to loop(), because deep sleep is a
// reset: there is no main loop on a device that is awake for a third of a
// second. loop() exists for the bench build, where the part stays up so a
// serial console can watch a pairing attempt happen.
//
// WHAT SURVIVES A SLEEP, AND WHERE
// --------------------------------
// RTC memory  the sequence number, the failure counters, and readings that
//             could not be delivered. Survives deep sleep, costs nothing to
//             write, and is gone on a power cut — which is correct for all
//             three.
// NVS         the collector's MAC, the channel, the node id, the access point
//             to look for. Survives everything, and is written only when the
//             collector tells us something new, because flash wears out.
//
// Nothing is written to flash on an ordinary wake. A node that stored its
// sequence number in NVS would do a minute-by-minute write for a year.
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "Link.h"
#include "node_config.h"
#include "src/drivers/BME280_Mini.h"
#include "src/espnow/EspNowProto.h"

// ---------------------------------------------------------------------------
// State that outlives the sleep
// ---------------------------------------------------------------------------

/// Distinguishes a deep-sleep wake from a cold boot. RTC memory is preserved
/// across the former and undefined after the latter, so without a marker the
/// first frame after a power-on carries whatever was in the SRAM.
static const uint32_t RTC_MAGIC = 0x4E4F4431;   // "NOD1"

RTC_DATA_ATTR static uint32_t s_rtcMagic;
RTC_DATA_ATTR static uint16_t s_seq;
RTC_DATA_ATTR static uint8_t  s_failStreak;    ///< consecutive unanswered wakes
RTC_DATA_ATTR static uint32_t s_lastScanS;     ///< epoch of the last channel scan
RTC_DATA_ATTR static uint32_t s_wakeCount;

/// Readings the node could not deliver.
///
/// This is what the wire format's fifteen-sample burst is for, and what the
/// clock in the ACK is for. A node that cannot reach its collector for twenty
/// minutes should not silently lose twenty minutes of weather; it should hand
/// them over when the link comes back, each with the time it was actually
/// taken.
RTC_DATA_ATTR static EnvSample s_buf[ESPNOW_MAX_SAMPLES];
RTC_DATA_ATTR static uint32_t  s_bufEpoch[ESPNOW_MAX_SAMPLES];
RTC_DATA_ATTR static uint8_t   s_bufCount;

// ---------------------------------------------------------------------------
// Persistent configuration
// ---------------------------------------------------------------------------

static Preferences s_prefs;
static NodeLink    s_link;

static void loadLink() {
    memset(&s_link, 0, sizeof(s_link));
    s_link.intervalS = NODE_INTERVAL_S;

    s_prefs.begin("espnow-node", true);
    s_link.nodeId    = s_prefs.getUChar("id", 0);
    s_link.channel   = s_prefs.getUChar("ch", 0);
    s_link.intervalS = s_prefs.getUShort("iv", NODE_INTERVAL_S);
    s_prefs.getBytes("mac", s_link.collector, 6);
    s_prefs.getBytes("bssid", s_link.bssid, 6);
    s_prefs.getString("ssid", s_link.ssid, sizeof(s_link.ssid));
    s_prefs.end();
}

static void saveLink() {
    s_prefs.begin("espnow-node", false);
    s_prefs.putUChar("id", s_link.nodeId);
    s_prefs.putUChar("ch", s_link.channel);
    s_prefs.putUShort("iv", s_link.intervalS);
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

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

/// Cell voltage in millivolts, or 0 when it cannot be measured.
///
/// analogReadMilliVolts() and not analogRead(): the ESP32-C3's ADC is neither
/// linear nor consistent part to part, and the raw counts are out by up to
/// 10 %. That function applies the calibration burned into the chip's eFuses
/// at the factory, which is the difference between a remaining-life estimate
/// that means something and one that is confidently wrong. NODE_BATT_TRIM is
/// there for what the resistors get wrong on top of that.
static uint16_t readBatteryMv() {
    analogSetPinAttenuation(NODE_BATT_PIN, ADC_11db);

    uint32_t sum = 0;
    for (int i = 0; i < NODE_BATT_SAMPLES; i++)
        sum += analogReadMilliVolts(NODE_BATT_PIN);

    const float atPin = (float)sum / (float)NODE_BATT_SAMPLES;
    const float mv    = atPin * NODE_BATT_DIVIDER * NODE_BATT_TRIM;

    // Nothing plausible is below a volt. A node with no divider fitted reads
    // near zero, and reporting that as a battery voltage would put a node at
    // 0 % on the collector's dashboard for ever.
    if (mv < 1000.0f || mv > 5000.0f) return 0;
    return (uint16_t)(mv + 0.5f);
}

// ---------------------------------------------------------------------------
// Sensor
// ---------------------------------------------------------------------------

static BME280_Mini s_bmx;
static bool        s_bmxOk = false;

static void sensorBegin() {
    Wire.begin(NODE_I2C_SDA, NODE_I2C_SCL);
    s_bmxOk = s_bmx.begin(NODE_BMX_ADDR, &Wire);
    if (!s_bmxOk) {
        // Most breakouts tie SDO low and answer at 0x76, a few do not. Trying
        // the other address costs a millisecond and saves somebody an evening.
        const uint8_t alt = (NODE_BMX_ADDR == 0x76) ? 0x77 : 0x76;
        s_bmxOk = s_bmx.begin(alt, &Wire);
        if (s_bmxOk) Serial.printf("[node] sensor answered at 0x%02X\n", alt);
    }
}

/// Fill one sample from whatever is actually available.
///
/// Every field starts absent and is only written when it was measured. A
/// BMP280 has no humidity sensor, and a 0 %RH on the wire would be a reading
/// the collector could not tell from a real one.
static void readSample(EnvSample& s, uint16_t vbat) {
    enClearSample(s);
    if (s_bmxOk) {
        s.t_c100   = enPackTemp(s_bmx.readTemperature());
        s.press_pa = enPackPress(s_bmx.readPressure() * 100.0f);   // hPa → Pa
        if (s_bmx.isBME280()) s.rh_x100 = enPackRh(s_bmx.readHumidity());
    }
    if (vbat) s.vbat_mv = enPackMv((float)vbat);
}

// ---------------------------------------------------------------------------
// The undelivered queue
// ---------------------------------------------------------------------------

static void bufferSample(const EnvSample& s, uint32_t epoch) {
    if (s_bufCount >= ESPNOW_MAX_SAMPLES) {
        // Full: drop the oldest. Losing the beginning of an outage is better
        // than losing the end of it — the recent readings are the ones that
        // say what the weather is doing now.
        memmove(&s_buf[0], &s_buf[1], sizeof(EnvSample) * (ESPNOW_MAX_SAMPLES - 1));
        memmove(&s_bufEpoch[0], &s_bufEpoch[1],
                sizeof(uint32_t) * (ESPNOW_MAX_SAMPLES - 1));
        s_bufCount = ESPNOW_MAX_SAMPLES - 1;
    }
    s_buf[s_bufCount]      = s;
    s_bufEpoch[s_bufCount] = epoch;
    s_bufCount++;
}

/// Build the frame: everything buffered, oldest first, then the live reading.
///
/// dt_s is computed here rather than stored, because it is an age and ages
/// change while a sample waits. A node with no clock sends 0 for everything
/// and the collector stamps on arrival, which is the honest answer — it does
/// not know when these were taken.
static uint8_t buildFrame(DataMsg& m, const EnvSample& live, uint32_t now,
                          bool coldBoot) {
    memset(&m, 0, sizeof(m));
    m.magic  = ESPNOW_MAGIC;
    m.ver    = ESPNOW_PROTO_VER;
    m.type   = EN_MSG_DATA;
    m.nodeId = s_link.nodeId;
    m.seq    = s_seq;
    m.epoch  = now;
    m.flags  = coldBoot ? EN_FLAG_FIRST_BOOT : 0;

    uint8_t n = 0;
    for (uint8_t i = 0; i < s_bufCount && n < ESPNOW_MAX_SAMPLES - 1; i++) {
        m.s[n] = s_buf[i];
        uint32_t age = 0;
        if (now && s_bufEpoch[i] && now > s_bufEpoch[i]) age = now - s_bufEpoch[i];
        m.s[n].dt_s = (age > 65535u) ? 65535u : (uint16_t)age;
        n++;
    }
    m.s[n] = live;
    m.s[n].dt_s = 0;
    n++;
    return n;
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

static void sleepNow(uint16_t seconds) {
    Serial.printf("[node] wake %u done in %lu ms, sleeping %us\n",
                  (unsigned)s_wakeCount, (unsigned long)millis(), seconds);
    Serial.flush();

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

void setup() {
    Serial.begin(115200);
#ifdef NODE_NO_DEEP_SLEEP
    delay(200);
    Serial.println("[node] BENCH BUILD — no deep sleep. Do not run this on a battery.");
#endif

    const bool coldBoot = (s_rtcMagic != RTC_MAGIC);
    if (coldBoot) {
        s_rtcMagic   = RTC_MAGIC;
        s_seq        = 0;
        s_failStreak = 0;
        s_lastScanS  = 0;
        s_wakeCount  = 0;
        s_bufCount   = 0;
    }
    s_wakeCount++;

    loadLink();

    // Measured before the radio comes up, while the board is drawing least.
    // Under the transmit burst the cell reads several tens of millivolts lower
    // and the reading would be about the antenna rather than the battery.
    const uint16_t vbat = readBatteryMv();

    sensorBegin();
    EnvSample live;
    readSample(live, vbat);

    if (!linkBegin(s_link)) {
        Serial.println("[node] radio failed to start");
        bufferSample(live, (uint32_t)time(nullptr));
        sleepNow(s_link.intervalS);
        return;
    }

    // ── Never provisioned: find a collector ─────────────────────────────────
    if (s_link.nodeId == 0) {
        Serial.println("[node] no collector known — sweeping for one");
        if (linkPair(s_link)) {
            saveLink();
            Serial.printf("[node] paired as node %u on channel %u\n",
                          s_link.nodeId, s_link.channel);
        } else {
            // The collector only answers while a pairing window is open, and
            // that window is opened by a human. Sleeping a full interval
            // between attempts is right: sweeping continuously would empty the
            // cell before anybody got to the collector.
            Serial.println("[node] nobody answered — is the collector's pairing window open?");
            linkEnd();
            bufferSample(live, (uint32_t)time(nullptr));
            sleepNow(s_link.intervalS);
            return;
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    const uint32_t now = (uint32_t)time(nullptr);
    DataMsg m;
    const uint8_t count = buildFrame(m, live, (now >= 1000000000u) ? now : 0, coldBoot);

    LinkResult r = linkSend(s_link, m, count);
    Serial.printf("[node] sent %u sample(s) seq=%u -> sent=%d ack=%d waited=%lums\n",
                  count, (unsigned)s_seq, (int)r.sent, (int)r.acked,
                  (unsigned long)r.waitedMs);

    if (r.acked) {
        s_seq++;
        s_failStreak = 0;
        s_bufCount   = 0;          // delivered; the queue is empty again

        // The only clock this node has. No NTP — it never associates — and no
        // 32 kHz crystal on this board, so deep sleep is timed by an RC
        // oscillator that drifts by percent. Resynchronising every wake is
        // what keeps a buffered burst's timestamps worth anything.
        if (r.epoch >= 1000000000u) {
            struct timeval tv = {};
            tv.tv_sec = (time_t)r.epoch;
            settimeofday(&tv, nullptr);
        }
        if (r.intervalS && r.intervalS != s_link.intervalS) {
            s_link.intervalS = r.intervalS;
            s_prefs.begin("espnow-node", false);
            s_prefs.putUShort("iv", s_link.intervalS);
            s_prefs.end();
            Serial.printf("[node] collector asked for %us\n", s_link.intervalS);
        }
        rememberChannel(r.channel);

        if (r.rediscover) {
            Serial.println("[node] collector no longer knows us — pairing again");
            if (linkPair(s_link)) saveLink();
        }
    } else {
        s_seq++;                   // the frame went out; do not reuse its number
        if (s_failStreak < 255) s_failStreak++;
        bufferSample(live, (now >= 1000000000u) ? now : 0);

        // ── Nobody answered. Go looking for where the network moved. ────────
        //
        // Rate limited, and the rate limit is measured from the last scan and
        // not from a clock: a channel change is recovered at THIS wake, not on
        // the hour. The ceiling exists for the other case — a collector simply
        // switched off — where scanning every minute would cost more radio
        // than reporting does.
        const bool due = s_failStreak >= NODE_RESCAN_FAILS &&
                         (s_lastScanS == 0 || now == 0 ||
                          (now - s_lastScanS) >= NODE_RESCAN_MIN_INTERVAL_S);

        if (due) {
            s_lastScanS = now;
            const uint8_t ch = linkFindChannel(s_link);
            if (ch && ch != s_link.channel) {
                Serial.printf("[node] access point moved to channel %u\n", ch);
                rememberChannel(ch);
            } else if (!ch) {
                // The access point is not on the air at all, or the collector
                // was reflashed and no longer holds our key. A reflashed
                // collector cannot even decrypt our frames — the radio drops
                // them before anything runs — so it will never ask us to
                // re-pair. Sweeping is the only way back, and it is cheap
                // enough once an hour.
                Serial.println("[node] cannot find the network — sweeping for a collector");
                NodeLink fresh = s_link;
                fresh.nodeId = 0;
                if (linkPair(fresh)) {
                    s_link = fresh;
                    saveLink();
                    s_failStreak = 0;
                    Serial.printf("[node] re-paired as node %u\n", s_link.nodeId);
                }
            }
        }
    }

    linkEnd();
    sleepNow(s_link.intervalS);
}

void loop() {
    // Unreachable: setup() always ends in deep sleep, and the bench build ends
    // in a restart. Present because Arduino requires it.
}
