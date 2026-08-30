#include "EspNowIngest.h"

#ifdef FEATURE_ESPNOW_INGEST

#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>
#include <Preferences.h>

#include "EspNowAuth.h"
#include "../core/EventLog.h"   // the clock-skew warning outlives the serial cable
#include "../core/Globals.h"    // bootCount, to tie a log line to a boot
#include "../sensors/RemoteIngest.h"

// ============================================================================
// WHICH TASK DOES WHAT, AND WHY IT MATTERS
// ============================================================================
// Two tasks touch the state in this file:
//
//   the WiFi task    runs onRecv(). Validates the frame, sends the ACK, and
//                    parks the rest. Nothing else.
//   an ordinary task runs espnowIngestTick() from loop(). Does all the
//                    thinking: sequence numbers, battery history, the
//                    handshake, the filesystem, and the handoff to
//                    RemoteIngest.
//
// The split is not stylistic. ESP-IDF's own documentation says the receive
// callback runs on the WiFi task and that lengthy work there is a mistake,
// and "lengthy" covers an HMAC, a flash write, and a walk of a 32-entry
// mailbox — all of which the naive version of this file did inside the
// callback.
//
// THE ONE EXCEPTION is the ACK, which IS sent from the callback. The node is
// holding its radio in receive waiting for it and every millisecond of that
// wait is battery it does not get back. Handing the reply to another task
// first would be tidier and would cost the node exactly the thing the reply
// exists to be. esp_now_send() queues to the driver and does not block, so it
// stays short.
//
// If that turns out to misbehave from inside the callback on real hardware,
// the fix is a small high-priority task fed by this same ring: the node
// tolerates tens of milliseconds, so a task hop is affordable. It is written
// this way first because it is simpler. This is the choice in this file most
// likely to need revisiting on a board.
//
// LOCKING
// -------
// One spinlock for the node table, one for the ring. Neither is ever held
// while the other is taken, and — the rule that matters — the node lock is
// never held across a RemoteIngest::put(), which takes a spinlock of its own.
// The tick does its integer bookkeeping under the lock, copies out what it
// needs, releases, and only then hands readings on.
// ============================================================================

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
// One shared key for every node, set at build time and flashed into both
// sides:
//
//   -DFEATURE_ESPNOW_INGEST -DESPNOW_LMK='"16-byte-secret!!"'
//
// A per-node key would be better — one compromised node would then not be
// every node — but it needs somewhere to store eight of them and a UI to
// enter them, and this feature has neither yet. For one to three nodes on a
// home network the shared key is the right trade, and it is written down here
// rather than left for someone to discover.
//
// The PMK encrypts the LMK on the air during peer setup. It is not a secret
// in the same sense — both ends must simply agree — but it must be 16 bytes.
#ifndef ESPNOW_LMK
#  define ESPNOW_LMK "change-this-key!"
#endif
#ifndef ESPNOW_PMK
#  define ESPNOW_PMK "esp32-logger-pmk"
#endif

// sizeof includes the terminator, so 17 is a 16-character key. A shorter one
// would read past the literal when copied into esp_now_peer_info_t::lmk.
static_assert(sizeof(ESPNOW_LMK) == 17, "ESPNOW_LMK must be exactly 16 characters");
static_assert(sizeof(ESPNOW_PMK) == 17, "ESPNOW_PMK must be exactly 16 characters");

// ESPNOW_BOOT_PAIRING_S now lives in EspNowIngest.h: the web interface's
// pairing button defaults to the same duration, and two copies of a number
// two files apart is one copy too many.

static const char* NODES_FILE = "/espnow_nodes.bin";

/// Bumped when EspNowNode's layout changes, so a stale file is discarded
/// rather than read as garbage. The file holds raw structs — it is internal,
/// small, and rewritten whole — so there is nothing to migrate.
static const uint32_t NODES_MAGIC = 0x454E3031;  // "EN01"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static EspNowNodeTable   s_nodes;
static portMUX_TYPE      s_nodeMux = portMUX_INITIALIZER_UNLOCKED;

static EspNowIngestStats s_stats;
static bool              s_up          = false;
static uint32_t          s_pairUntilMs = 0;
static bool              s_dirty       = false;   ///< table changed, needs saving
static uint32_t          s_lastDay     = 0;       ///< last day written to flash
static uint8_t           s_offlineIntervals = 0;  // 0 = use ESPNOW_OFFLINE_INTERVALS

/// Frames the callback parked for the tick to deal with.
///
/// Four slots: several nodes reporting on the same second is the only way to
/// fill it, and the tick runs every loop() iteration. Overflow is counted
/// rather than silently dropped, because "the ring filled" and "nothing ever
/// arrived" are very different problems with the same symptom.
struct RxFrame {
    uint8_t mac[6];   ///< kept for the log line when a frame names no known node
    DataMsg msg;
};
static const int    RX_RING = 4;
static RxFrame      s_ring[RX_RING];
static int          s_ringHead = 0;    ///< advanced by the tick
static int          s_ringTail = 0;    ///< advanced by the callback
static portMUX_TYPE s_ringMux  = portMUX_INITIALIZER_UNLOCKED;

/// One pending DISCOVER, waiting for the tick to verify its signature.
///
/// One slot and not a queue: pairing is a human-initiated, one-node-at-a-time
/// act, and a node that is ignored because another was mid-handshake simply
/// sweeps and tries again. A queue here would be machinery for a case that
/// does not happen.
static DiscoverMsg s_pendingDiscover;
static uint8_t     s_pendingMac[6];
static volatile bool s_pendingValid = false;

// ---------------------------------------------------------------------------
// How far each node's clock is from ours
// ---------------------------------------------------------------------------
// Measured HERE and not reported by the node, because the node is the one with
// a battery. Every DATA frame already carries the node's own epoch, and this
// collector has a real clock, so the difference is free: no extra byte on the
// air, no extra wake, and — the part that matters on a device that deep sleeps
// — not one flash write on the node to record something only readable with a
// cable attached.
//
// Kept beside the table rather than inside EspNowNode. Adding a field there
// changes sizeof(), which makes loadNodes() discard the saved file and costs
// every deployed node a re-pair. A live measurement retaken on the next report
// has no business surviving a reboot at that price.
//
// Positive means the node is BEHIND us, which is the direction an RC-timed
// deep sleep drifts.
static int32_t s_skew[EspNowNodeTable::CAP]      = {0};
static bool    s_haveSkew[EspNowNodeTable::CAP]  = {false};
/// millis() of the last log line per node, so a badly drifting node writes
/// one line an hour and not one a minute.
static uint32_t s_skewLoggedMs[EspNowNodeTable::CAP] = {0};

#ifndef ESPNOW_SKEW_WARN_S
#  define ESPNOW_SKEW_WARN_S 60
#endif
#ifndef ESPNOW_SKEW_LOG_INTERVAL_MS
#  define ESPNOW_SKEW_LOG_INTERVAL_MS 3600000UL
#endif

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static inline uint32_t nowEpoch() {
    const uint32_t t = (uint32_t)time(nullptr);
    // Below this the clock has never been set, and telling a node it is 1970
    // is worse than telling it nothing: it would timestamp a buffered burst
    // with a date that is then thrown away at the far end.
    return (t >= 1000000000u) ? t : 0;
}

static inline uint8_t currentChannel() {
    uint8_t ch = 0;
    wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return 0;
    return ch;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
// The node table has to survive a reboot or provisioning is lost every time
// the power blinks — and with it the battery history, which takes five days
// to rebuild before batteryDaysLeft() will answer again.
//
// Written whole, never in place: eight structs is well under a flash page,
// and a full rewrite cannot leave a half-updated record behind.
//
// Called only from the tick, so the node lock is taken here rather than by
// the caller. Nothing else in this file writes the filesystem.

// Persisting the node table, with a retry that is not a spin.
//
// s_dirty stays set when the write fails, which is right — the table still
// needs saving — but the tick runs from loop(), so "still needs saving" meant
// retrying the open thousands of times a second and printing a line each time.
// A full or broken filesystem does not heal inside a millisecond, and the log
// it produced would bury whatever else was wrong.
//
// So a failure buys a minute of quiet. The flag is untouched, so the retry
// still happens; it just happens at a rate that matches how fast a filesystem
// fault can plausibly clear.
static uint32_t s_saveRetryAtMs = 0;   ///< millis() before which not to retry

static bool saveNodes() {
    // Signed difference, so this stays correct across the millis() wrap.
    if (s_saveRetryAtMs != 0 && (int32_t)(s_saveRetryAtMs - millis()) > 0) return false;

    EspNowNode snap[EspNowNodeTable::CAP];
    taskENTER_CRITICAL(&s_nodeMux);
    for (int i = 0; i < EspNowNodeTable::CAP; i++) snap[i] = s_nodes.at(i);
    taskEXIT_CRITICAL(&s_nodeMux);

    File f = LittleFS.open(NODES_FILE, "w");
    if (!f) {
        Serial.println("[ESPNOW] could not open the node file for writing "
                       "— retrying in 60 s");
        s_saveRetryAtMs = millis() + 60000u;
        if (s_saveRetryAtMs == 0) s_saveRetryAtMs = 1;   // 0 means "no backoff"
        return false;
    }
    f.write((const uint8_t*)&NODES_MAGIC, sizeof(NODES_MAGIC));
    for (int i = 0; i < EspNowNodeTable::CAP; i++)
        f.write((const uint8_t*)&snap[i], sizeof(snap[i]));
    f.close();
    s_dirty = false;
    s_saveRetryAtMs = 0;
    return true;
}

static void loadNodes() {
    if (!LittleFS.exists(NODES_FILE)) return;

    File f = LittleFS.open(NODES_FILE, "r");
    if (!f) return;

    const size_t want = sizeof(NODES_MAGIC) +
                        sizeof(EspNowNode) * (size_t)EspNowNodeTable::CAP;
    if (f.size() != want) {
        // A layout change, not corruption. Discard rather than reinterpret:
        // reading one struct as another produces nodes with plausible ids and
        // nonsense battery histories, which is worse than starting over.
        Serial.printf("[ESPNOW] node file is %u bytes, expected %u — discarding\n",
                      (unsigned)f.size(), (unsigned)want);
        f.close();
        LittleFS.remove(NODES_FILE);
        return;
    }

    uint32_t magic = 0;
    f.read((uint8_t*)&magic, sizeof(magic));
    if (magic != NODES_MAGIC) {
        Serial.println("[ESPNOW] node file magic mismatch — discarding");
        f.close();
        LittleFS.remove(NODES_FILE);
        return;
    }

    for (int i = 0; i < EspNowNodeTable::CAP; i++) {
        EspNowNode n{};
        f.read((uint8_t*)&n, sizeof(n));
        if (!n.used) continue;
        // lastSeenMs came from a previous boot's millis() and means nothing
        // now. Clearing everSeen is what makes a restored node read as
        // offline until it actually reports, rather than as freshly heard.
        n.lastSeenMs = 0;
        n.everSeen   = false;
        n.haveSeq    = false;
        taskENTER_CRITICAL(&s_nodeMux);
        s_nodes.at(i) = n;
        taskEXIT_CRITICAL(&s_nodeMux);
    }
    f.close();
    Serial.printf("[ESPNOW] restored %d node(s)\n", s_nodes.count());
}

// ---------------------------------------------------------------------------
// Peers
// ---------------------------------------------------------------------------

static bool addPeer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    memcpy(p.lmk, ESPNOW_LMK, 16);
    // Channel 0 means "whatever the interface is on". The collector follows
    // its access point, so pinning a number here would break every peer the
    // first time the router moved.
    p.channel = 0;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = true;

    const esp_err_t rc = esp_now_add_peer(&p);
    if (rc != ESP_OK) {
        Serial.printf("[ESPNOW] add_peer failed: %d\n", (int)rc);
        return false;
    }
    return true;
}

/// The broadcast pseudo-peer. ESP-NOW will not send to an address it has not
/// been told about, and that includes ff:ff:ff:ff:ff:ff. Unencrypted of
/// necessity: a broadcast has no peer to hold a key.
static bool addBroadcastPeer() {
    static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (esp_now_is_peer_exist(BCAST)) return true;

    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = 0;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

// ---------------------------------------------------------------------------
// The receive callback — WiFi task
// ---------------------------------------------------------------------------

/// Answer a DATA frame.
///
/// A frame from a node we do not know still gets an answer, and it carries
/// EN_ACK_REDISCOVER: that is what sends a node whose collector was reflashed
/// back through pairing, instead of leaving it transmitting for months into
/// something that will never decode it.
static void sendAck(const uint8_t* mac, const DataMsg& m) {
    bool     known    = false;
    uint16_t interval = 0;

    taskENTER_CRITICAL(&s_nodeMux);
    const EspNowNode* n = s_nodes.byId(m.nodeId);
    if (n) { known = true; interval = n->intervalS; }
    taskEXIT_CRITICAL(&s_nodeMux);

    AckMsg a{};
    a.magic     = ESPNOW_MAGIC;
    a.ver       = ESPNOW_PROTO_VER;
    a.type      = EN_MSG_ACK;
    a.nodeId    = m.nodeId;
    a.ackSeq    = m.seq;
    a.flags     = known ? 0 : EN_ACK_REDISCOVER;
    a.epoch     = nowEpoch();
    a.intervalS = interval;
    a.channel   = currentChannel();

    if (esp_now_send(mac, (const uint8_t*)&a, sizeof(a)) == ESP_OK) s_stats.acksSent++;
}

#if ESP_IDF_VERSION_MAJOR >= 5
// Arduino core 3.x / IDF 5 replaced the bare MAC with a struct that also
// carries the signal strength. This firmware pins core 2.0.17, so this branch
// is compiled only by the core-3 probe environment — it is here so that
// migrating is a rebuild rather than a debugging session.
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* mac = info ? info->src_addr : nullptr;
    const int      rssi = (info && info->rx_ctrl) ? info->rx_ctrl->rssi : 0;
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    // IDF 4.4 hands the callback no signal information at all. Left at zero
    // rather than filled with a guess: EspNowNode::rssi reading 0 means "this
    // build cannot tell you", which is true.
    const int rssi = 0;
#endif
    if (!mac) return;

    uint8_t type = 0;
    if (!espnowValidate(data, len, type)) { s_stats.malformed++; return; }

    if (type == EN_MSG_DISCOVER) {
        s_stats.discoverSeen++;
        // Parked, not verified: the HMAC is tens of microseconds of SHA-256
        // and this is the WiFi task. The tick does the arithmetic.
        if (!s_pendingValid) {
            memcpy(&s_pendingDiscover, data, sizeof(s_pendingDiscover));
            memcpy(s_pendingMac, mac, 6);
            s_pendingValid = true;
        }
        return;
    }
    if (type != EN_MSG_DATA) {
        // ACK and WELCOME are ours to send, not to receive. A collector
        // hearing one is either talking to itself or hearing a second
        // collector; there is nothing useful to do with it either way.
        return;
    }

    DataMsg m;
    espnowDecodeData(data, len, m);

    // QUEUE FIRST, ACK SECOND, and the order is the whole point.
    //
    // The ACK is what tells the node its report landed; on receiving one the
    // node clears the samples it was carrying — up to fifteen of them, an
    // outage's worth. Acknowledging before the enqueue meant a ring-full drop
    // still said "landed", and the node then threw away readings that had
    // reached no further than this function. A silent hole in the record,
    // visible only as a ringFull counter nobody was watching.
    //
    // Unacknowledged, the node keeps its buffer and brings the samples again
    // on its next wake, which is the right trade: a lost reading cannot be
    // recovered and a repeated one can be lived with.
    //
    // BUT IT IS NOT FREE, and an earlier version of this comment said it was.
    // espnowSeqCheck() drops a repeated SEQUENCE NUMBER, and the node does not
    // repeat one — main.cpp increments s_seq on the unacknowledged path too,
    // then re-sends the buffered samples inside a later frame alongside newer
    // ones. So the retransmission is not a duplicate frame the collector could
    // recognise; it is a new frame whose contents overlap.
    //
    // What that costs: samples from a frame that landed but whose ACK was lost
    // in the air are stored twice. The trend grid buckets by the reading's own
    // timestamp, so a repeat lands in the hour it belongs to and overwrites
    // rather than double-counting; the CSV gets two rows with the same stamp.
    // Deduplicating properly needs per-sample identity rather than per-frame,
    // which the wire format does not carry today.
    taskENTER_CRITICAL(&s_ringMux);
    const int next = (s_ringTail + 1) % RX_RING;
    if (next == s_ringHead) {
        taskEXIT_CRITICAL(&s_ringMux);
        s_stats.ringFull++;
        return;
    }
    memcpy(s_ring[s_ringTail].mac, mac, 6);
    s_ring[s_ringTail].msg = m;
    s_ringTail = next;
    taskEXIT_CRITICAL(&s_ringMux);

    // Safely outside the spinlock, and only now that the frame is somewhere
    // the tick will find it.
    sendAck(mac, m);

    if (rssi) {
        taskENTER_CRITICAL(&s_nodeMux);
        EspNowNode* n = s_nodes.byMac(mac);
        if (n) n->rssi = (int8_t)rssi;
        taskEXIT_CRITICAL(&s_nodeMux);
    }
}

// ---------------------------------------------------------------------------
// The tick — ordinary task
// ---------------------------------------------------------------------------

/// Verify and complete a parked handshake. Returns true if a node was added.
static bool servicePendingDiscover() {
    if (!s_pendingValid) return false;

    DiscoverMsg d;
    uint8_t     mac[6];
    memcpy(&d, &s_pendingDiscover, sizeof(d));
    memcpy(mac, s_pendingMac, 6);
    s_pendingValid = false;      // released before the work, so a retry can park

    if (!espnowPairingActive()) return false;

    // Verified with the shared helper, against exactly the region the node
    // signed with the same one. Two implementations of this is how a node
    // pairs with nothing while both sides look correct in isolation.
    if (!espnowVerifyTag((const uint8_t*)ESPNOW_LMK, (const uint8_t*)&d,
                         EN_DISCOVER_SIGNED_LEN, d.tag)) {
        s_stats.discoverBadSig++;
        return false;
    }

    uint8_t  assigned = 0;
    uint16_t interval = ESPNOW_DEFAULT_INTERVAL_S;
    taskENTER_CRITICAL(&s_nodeMux);
    // Reuse the slot this MAC already holds, so re-pairing a node the
    // collector already knows does not consume a second id.
    const EspNowNode* n = s_nodes.byMac(mac);
    if (!n) {
        for (uint8_t id = 1; id < 255 && !n; id++)
            if (!s_nodes.byId(id))
                n = s_nodes.add(mac, id, nullptr, ESPNOW_DEFAULT_INTERVAL_S);
    }
    if (n) { assigned = n->nodeId; interval = n->intervalS; }
    taskEXIT_CRITICAL(&s_nodeMux);

    if (!assigned) {
        Serial.println("[ESPNOW] pairing refused: node table is full");
        return false;
    }

    WelcomeMsg w{};
    // The node's OWN interval, not the default. Re-pairing a node whose wake
    // period had been changed would otherwise hand it the default back while
    // the collector's table went on expecting the configured one — so the node
    // would report on one schedule and be judged offline against another.
    w.intervalS = interval;
    w.epoch     = nowEpoch();
    w.channel   = currentChannel();

    // Which access point to look for when the channel moves. BSSID is exact;
    // SSID is the fallback for a mesh that changes the BSSID under you.
    const uint8_t* bssid = WiFi.BSSID();
    if (bssid) memcpy(w.bssid, bssid, 6);
    strncpy(w.ssid, WiFi.SSID().c_str(), sizeof(w.ssid) - 1);

    if (!espnowSignWelcome(w, (const uint8_t*)ESPNOW_LMK, assigned, mac))
        return false;

    // BROADCAST, AND BEFORE THE PEER IS ADDED. Both matter. The node cannot
    // receive an encrypted frame from a collector it has not yet added as a
    // peer, and it cannot add one until this message tells it the collector's
    // MAC — there is no ordering that resolves that, so the reply goes out in
    // the clear, signed, addressed by the target field inside it.
    //
    // Adding the encrypted peer afterwards is what makes every subsequent
    // frame encrypted: once the peer exists with encrypt=true, sends to it
    // are, and this broadcast would not have been.
    static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(BCAST, (const uint8_t*)&w, sizeof(w));

    if (!addPeer(mac)) return false;
    s_stats.paired++;
    s_dirty = true;
    Serial.printf("[ESPNOW] paired node %u on channel %u\n", assigned, w.channel);
    return true;
}

/// Everything one frame implies for the node table, done under the lock.
///
/// Returns false when the frame should be dropped. On success `outId` holds
/// the node's pipeline id and `outSnap` a copy of the node — the caller needs
/// both AFTER releasing the lock, because RemoteIngest::put() takes a
/// spinlock of its own and nesting the two is how lock-ordering bugs are
/// born.
static bool acceptFrame(const RxFrame& f, char* outId, size_t outIdLen,
                        EspNowNode& outSnap) {
    bool ok = false;

    // Filled under the lock, acted on after it is released: the log line writes
    // flash, which a critical section is the last place to do.
    bool    logSkew     = false;
    int32_t logSkewVal  = 0;
    char    logSkewId[sizeof(EspNowNode::id)] = {0};

    // Read once, out here. time() is a libc call that takes a lock of its own,
    // and the critical section below runs with interrupts off — the fewer
    // things reached from inside it, the better. It was already being called
    // in there for `base`; now it is called once, for both.
    const uint32_t ours = nowEpoch();

    taskENTER_CRITICAL(&s_nodeMux);
    EspNowNode* n = s_nodes.byId(f.msg.nodeId);
    if (n) {
        const EspNowSeqVerdict v =
            espnowSeqCheck(n->haveSeq, n->lastSeq, f.msg.seq, f.msg.flags);
        if (v == EN_SEQ_DUPLICATE || v == EN_SEQ_STALE) {
            n->framesDropped++;
        } else {
            n->haveSeq    = true;
            n->lastSeq    = f.msg.seq;
            n->lastSeenMs = millis();
            n->everSeen   = true;
            n->framesRx++;

            // How far this node's clock is from ours, when both sides have one.
            // The arithmetic and its two guards live in NodeTable.h, where the
            // host tests can reach them.
            int32_t skew = 0;
            if (espnowClockSkew(ours, f.msg.epoch, skew)) {
                const int idx = s_nodes.indexOf(f.msg.nodeId);
                if (idx >= 0) {
                    const int32_t mag = skew < 0 ? -skew : skew;
                    // Log on crossing the threshold, at most once an hour per
                    // node. Unsigned difference, so it stays right across the
                    // millis() wrap; the zero initial value makes the first
                    // offence log immediately rather than an hour into uptime.
                    if (mag >= (int32_t)ESPNOW_SKEW_WARN_S &&
                        (s_skewLoggedMs[idx] == 0 ||
                         (uint32_t)(millis() - s_skewLoggedMs[idx]) >= ESPNOW_SKEW_LOG_INTERVAL_MS)) {
                        s_skewLoggedMs[idx] = millis();
                        if (s_skewLoggedMs[idx] == 0) s_skewLoggedMs[idx] = 1;  // 0 means "never"
                        logSkew    = true;
                        logSkewVal = skew;
                        strncpy(logSkewId, n->id, sizeof(logSkewId) - 1);
                    }
                    s_skew[idx]     = skew;
                    s_haveSkew[idx] = true;
                }
            }

            const uint32_t base = f.msg.epoch ? f.msg.epoch : ours;
            for (uint8_t i = 0; i < f.msg.count && i < ESPNOW_MAX_SAMPLES; i++) {
                const float mv = enUnpackMv(f.msg.s[i].vbat_mv);
                if (enIsAbsent(mv)) continue;
                n->lastMv = (uint16_t)mv;
                const uint32_t ts = (base > f.msg.s[i].dt_s) ? base - f.msg.s[i].dt_s
                                                             : base;
                batteryHistoryAdd(n->batt, ts, n->lastMv);
            }

            strncpy(outId, n->id, outIdLen - 1);
            outId[outIdLen - 1] = '\0';
            outSnap = *n;
            ok = true;
        }
    }
    taskEXIT_CRITICAL(&s_nodeMux);

    // A minute out is not a rounding error on a node that reports every minute:
    // it means either that the node has been running on its RC oscillator since
    // the last ACK it actually heard, or that the collector's own clock jumped.
    // Either way the backfill timestamps in that frame are a minute wrong, and
    // the only way anyone finds out is if it is written down — the serial
    // console is not attached to a device that is outdoors.
    if (logSkew) {
        Serial.printf("[ESPNOW] node %s clock is %ld s %s\n",
                      logSkewId, (long)(logSkewVal < 0 ? -logSkewVal : logSkewVal),
                      logSkewVal > 0 ? "behind" : "ahead");
        eventLogPrintf("boot#%u  ESPNOW_SKEW  node=%s  skew=%+ld s",
                       (unsigned)bootCount, logSkewId, (long)logSkewVal);
    }

    if (!n) {
        s_stats.unknownNode++;
        // Worth a log line and not just a counter: this is what a node whose
        // collector was reflashed looks like, and the MAC is what you need to
        // pair it again.
        Serial.printf("[ESPNOW] frame from unprovisioned node %u "
                      "(%02x:%02x:%02x:%02x:%02x:%02x)\n",
                      f.msg.nodeId, f.mac[0], f.mac[1], f.mac[2],
                      f.mac[3], f.mac[4], f.mac[5]);
    } else if (!ok) {
        s_stats.replayed++;
    } else {
        s_stats.framesRx++;
    }
    return ok;
}

void espnowIngestTick() {
    if (!s_up) return;

    servicePendingDiscover();

    for (;;) {
        RxFrame f;
        taskENTER_CRITICAL(&s_ringMux);
        if (s_ringHead == s_ringTail) { taskEXIT_CRITICAL(&s_ringMux); break; }
        f = s_ring[s_ringHead];
        s_ringHead = (s_ringHead + 1) % RX_RING;
        taskEXIT_CRITICAL(&s_ringMux);

        char       id[sizeof(EspNowNode::id)];
        EspNowNode snap{};
        if (!acceptFrame(f, id, sizeof(id), snap)) continue;

        // The node's own clock is used when it has one, and it is load-bearing:
        // SensorManager stamps only a reading whose timestamp is zero, so what
        // is derived here is the time the reading is filed under. Zero when
        // neither side has a clock, which the history path below tests for.
        const uint32_t base = f.msg.epoch ? f.msg.epoch : nowEpoch();

        // WHERE EACH SAMPLE GOES
        //
        // The newest goes through RemoteIngest::put() — the mailbox slot, the
        // live value, overwritten by whatever comes next. That is what the
        // dashboard reads.
        //
        // Everything the node buffered through an outage goes through
        // putHistorical() instead, which queues rather than overwrites. Fed
        // through put() they would have collapsed into the one slot and
        // fourteen of fifteen readings would have been lost microseconds after
        // arriving — which is what this code did until the timestamps were
        // made authoritative end to end.
        //
        // Their timestamps are the node's own and survive: SensorManager only
        // stamps a reading whose timestamp is zero, and ProcessingTask keeps
        // backfilled readings out of the live ring and out of alert
        // evaluation while still folding them into storage and the trend grid.
        const uint8_t newest = (uint8_t)(f.msg.count - 1);

        for (uint8_t i = 0; i < f.msg.count && i < ESPNOW_MAX_SAMPLES; i++) {
            const EnvSample& s = f.msg.s[i];
            const uint32_t ts = (base > s.dt_s) ? base - s.dt_s : base;

            EspNowMetric m[EN_MAX_SAMPLE_METRICS];
            const int cnt = espnowExpandSample(s, m, EN_MAX_SAMPLE_METRICS);
            for (int k = 0; k < cnt; k++) {
                if (i == newest) {
                    remoteIngest.put(id, m[k].metric, m[k].value, m[k].unit, ts);
                } else if (ts < 1000000000u) {
                    // No clock anywhere — not on the node, not here — so a
                    // backdated reading has no date to be filed under and
                    // putHistorical() would refuse it. Counted separately
                    // from the overflow below, because the two ask for
                    // opposite things: one wants a bigger queue, this one
                    // wants NTP or an RTC.
                    s_stats.historyNoClock++;
                } else if (!remoteIngest.putHistorical(id, m[k].metric, m[k].value,
                                                       m[k].unit, ts)) {
                    // The queue was full and shed its oldest entry to take
                    // this one. Counted, because a backlog that keeps
                    // overflowing means the drain is too slow for the burst
                    // size and that is a tuning fact, not a mystery.
                    s_stats.historyCollapsed++;
                }
            }
        }

        // The two figures the collector derives rather than receives,
        // recomputed from the history the samples above just extended.
        // Emitted here rather than per-sample because they are properties of
        // the node, not of any one reading.
        EspNowMetric d[EN_MAX_NODE_METRICS];
        const int dn = espnowBatteryMetrics(snap, d, EN_MAX_NODE_METRICS);
        for (int k = 0; k < dn; k++)
            remoteIngest.put(id, d[k].metric, d[k].value, d[k].unit, base);
    }

    // Persist on a provisioning change, and once a day so a reboot does not
    // cost the battery history. Not per frame: that would be a flash write a
    // minute per node, which is how a partition gets worn out.
    // s_lastDay advances only on a write that HAPPENED. It used to be set
    // first, which quietly turned the retry off: saveNodes() can return early
    // on its 60 s backoff, and on the daily path s_dirty is false, so nothing
    // would have brought the tick back. One skipped write and that day's
    // battery minima were gone — the history that takes five days to rebuild
    // before batteryDaysLeft() will answer at all.
    const uint32_t today = nowEpoch() / 86400u;
    if (s_dirty || (today != 0 && today != s_lastDay && s_nodes.count() > 0)) {
        if (saveNodes()) s_lastDay = today;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle and accessors
// ---------------------------------------------------------------------------

bool espnowIngestBegin() {
    if (s_up) return true;

    // The radio has to be a station before ESP-NOW can ride it. This does not
    // start a connection — that is WiFiManager's job — it refuses to proceed
    // only if the interface is not there at all.
    if (WiFi.getMode() == WIFI_OFF) {
        Serial.println("[ESPNOW] WiFi is off — not starting");
        return false;
    }

    // MODEM SLEEP OFF, SAID OUT LOUD RATHER THAN ASSUMED.
    //
    // WIFI_PS_MIN_MODEM breaks ESP-NOW unicast while leaving broadcast
    // working, which is the failure this whole file's header warns about:
    // pairing succeeds, the node reports for years, and not one reading ever
    // arrives.
    //
    // modemSleepAllowed() in the sketch stops US from ENABLING it. That is
    // not the same as it being off: the Arduino-ESP32 default for a connected
    // station is WIFI_PS_MIN_MODEM, and nothing was ever turning it back off.
    // So the documented failure was not an edge case somebody might configure
    // their way into — it was the state the collector booted in.
    //
    // The node has always done this in linkBegin(). The collector now does the
    // same thing at the same point, and here rather than in the sketch because
    // this is the feature that requires it: a WiFi reconnect elsewhere cannot
    // quietly undo a decision made next to the radio that depends on it.
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK)
        Serial.println("[ESPNOW] could not turn modem sleep off — "
                       "unicast may not arrive");

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] esp_now_init() failed");
        return false;
    }
    if (esp_now_set_pmk((const uint8_t*)ESPNOW_PMK) != ESP_OK)
        Serial.println("[ESPNOW] set_pmk() failed");

    esp_now_register_recv_cb(onRecv);
    addBroadcastPeer();

    loadNodes();
    {
        Preferences prefs;
        prefs.begin("espnow", true);
        s_offlineIntervals = prefs.getUChar("offline_iv", 0);
        prefs.end();
    }
    for (int i = 0; i < EspNowNodeTable::CAP; i++) {
        const EspNowNode& n = s_nodes.at(i);
        if (n.used) addPeer(n.mac);
    }

    // A collector that knows no nodes listens for one. See the comment on
    // ESPNOW_BOOT_PAIRING_S: this is the entire provisioning interface until
    // there is a button for it.
    if (s_nodes.count() == 0) espnowBeginPairing(ESPNOW_BOOT_PAIRING_S);

    s_up = true;
    Serial.printf("[ESPNOW] up on channel %u, %d node(s)%s\n",
                  currentChannel(), s_nodes.count(),
                  espnowPairingActive() ? ", pairing open" : "");
    return true;
}

void espnowBeginPairing(uint32_t seconds) {
    s_pairUntilMs = millis() + seconds * 1000u;
    if (s_pairUntilMs == 0) s_pairUntilMs = 1;   // 0 is the "never opened" marker
    Serial.printf("[ESPNOW] pairing window open for %us\n", (unsigned)seconds);
}

bool espnowPairingActive() {
    if (s_pairUntilMs == 0) return false;
    // Signed difference, so the comparison stays correct across the millis()
    // wrap: a window opened just before it must not read as already expired.
    return (int32_t)(s_pairUntilMs - millis()) > 0;
}

// Either both halves take, or neither does.
//
// This used to mutate the table and then add the radio peer, so a peer that
// could not be added left the table already changed: a rename applied in RAM,
// s_dirty never set so it was never written, and the API returning an error
// over the top of it. The page then showed the new name until the next reboot
// showed the old one.
//
// The peer goes first now, because it is the half that can fail for reasons
// outside this table — the driver's peer list is a fixed twenty slots. And if
// the table refuses afterwards, a peer this call created is taken back out;
// one that was already there is left alone, since renaming an existing node
// must not cost it its peer.
bool espnowAddNode(const uint8_t mac[6], uint8_t nodeId, const char* label,
                   uint16_t intervalS) {
    const bool peerExisted = s_up && esp_now_is_peer_exist(mac);
    if (s_up && !addPeer(mac)) return false;

    taskENTER_CRITICAL(&s_nodeMux);
    const bool ok = s_nodes.add(mac, nodeId, label, intervalS) != nullptr;
    // A slot is reused, so whatever the previous occupant's clock was doing is
    // not this node's measurement. add() also returns the existing entry when
    // the node is merely being renamed — clearing there costs one report.
    if (ok) {
        const int idx = s_nodes.indexOf(nodeId);
        if (idx >= 0) { s_haveSkew[idx] = false; s_skew[idx] = 0; s_skewLoggedMs[idx] = 0; }
    }
    taskEXIT_CRITICAL(&s_nodeMux);

    if (!ok) {
        if (s_up && !peerExisted) esp_now_del_peer(mac);
        return false;
    }
    s_dirty = true;
    return true;
}

bool espnowRemoveNode(uint8_t nodeId) {
    uint8_t mac[6];
    bool found = false;

    taskENTER_CRITICAL(&s_nodeMux);
    const EspNowNode* n = s_nodes.byId(nodeId);
    if (n) {
        memcpy(mac, n->mac, 6);
        // Before remove(), while the slot still answers to this node id.
        const int idx = s_nodes.indexOf(nodeId);
        if (idx >= 0) { s_haveSkew[idx] = false; s_skew[idx] = 0; s_skewLoggedMs[idx] = 0; }
        s_nodes.remove(nodeId);
        found = true;
    }
    taskEXIT_CRITICAL(&s_nodeMux);

    if (!found) return false;
    if (s_up) esp_now_del_peer(mac);
    s_dirty = true;
    return true;
}

int espnowCopyNodes(EspNowNode* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    int n = 0;
    taskENTER_CRITICAL(&s_nodeMux);
    for (int i = 0; i < EspNowNodeTable::CAP && n < maxOut; i++)
        if (s_nodes.at(i).used) out[n++] = s_nodes.at(i);
    taskEXIT_CRITICAL(&s_nodeMux);
    return n;
}

bool espnowNodeSkew(uint8_t nodeId, int32_t& outSkewS) {
    bool have = false;
    // The index lookup and the read of the parallel array have to happen under
    // the same lock: a remove() between them would have this reading a slot
    // that now belongs to a different node, or to none.
    taskENTER_CRITICAL(&s_nodeMux);
    const int idx = s_nodes.indexOf(nodeId);
    if (idx >= 0 && s_haveSkew[idx]) { outSkewS = s_skew[idx]; have = true; }
    taskEXIT_CRITICAL(&s_nodeMux);
    return have;
}

bool espnowAnyBatteryWarn() {
    taskENTER_CRITICAL(&s_nodeMux);
    const bool warn = s_nodes.anyBatteryWarn();
    taskEXIT_CRITICAL(&s_nodeMux);
    return warn;
}

int espnowOfflineCount() {
    const uint32_t now = millis();
    taskENTER_CRITICAL(&s_nodeMux);
    const int c = s_nodes.offlineCount(now, espnowGetOfflineIntervals());
    taskEXIT_CRITICAL(&s_nodeMux);
    return c;
}

const EspNowIngestStats& espnowStats() { return s_stats; }
uint8_t espnowGetOfflineIntervals() {
    return s_offlineIntervals ? s_offlineIntervals : (uint8_t)ESPNOW_OFFLINE_INTERVALS;
}

void espnowSetOfflineIntervals(uint8_t n) {
    s_offlineIntervals = n;
    // Persist to NVS
    Preferences prefs;
    prefs.begin("espnow", false);
    prefs.putUChar("offline_iv", n);
    prefs.end();
}

#endif  // FEATURE_ESPNOW_INGEST
