#include "Link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "node_config.h"
#include "src/espnow/EspNowAuth.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// All of it lives for one wake. The callbacks run on the WiFi task and the
// rest on the Arduino task, so the handoff is a semaphore and a few fields
// written before it is given — which is the ordering that makes them visible.

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static SemaphoreHandle_t s_replySem = nullptr;
static uint8_t           s_ownMac[6];

static volatile bool s_sendDone = false;
static volatile bool s_sendOk   = false;

/// Filled by the receive callback before it gives the semaphore.
static AckMsg     s_ack;
static WelcomeMsg s_welcome;
static uint8_t    s_collectorMac[6];
static volatile bool s_haveAck     = false;
static volatile bool s_haveWelcome = false;

static uint16_t s_wantSeq = 0;   ///< the sequence number we are waiting on

/// A CFG is accepted only while linkExchangeCfg() is waiting for one, only
/// from the collector's MAC and only for our node id. It is the one frame
/// that can change what this node does, so "it decrypted" is not the only
/// check it gets.
static volatile bool s_wantCfg = false;
static volatile bool s_haveCfg = false;
static CfgChunkMsg   s_cfg;
static uint8_t       s_cfgFrom[6];
static uint8_t       s_cfgNode = 0;

/// The link key (see linkSetKey()). Starts as the compiled one so a caller
/// that forgets to set it gets today's behaviour, not a zero key.
static uint8_t s_key[16];
static bool    s_keySet = false;

void linkSetKey(const char* key16) {
    memcpy(s_key, key16, 16);
    s_keySet = true;
}

static const uint8_t* key() {
    if (!s_keySet) linkSetKey(ESPNOW_LMK);
    return s_key;
}

// ---------------------------------------------------------------------------
// Callbacks — WiFi task
// ---------------------------------------------------------------------------

static void onSent(const uint8_t*, esp_now_send_status_t status) {
    s_sendOk   = (status == ESP_NOW_SEND_SUCCESS);
    s_sendDone = true;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    const uint8_t* mac = info ? info->src_addr : nullptr;
#else
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (!mac) return;

    uint8_t type = 0;
    if (!espnowValidate(data, len, type)) return;

    if (type == EN_MSG_ACK) {
        AckMsg a;
        memcpy(&a, data, sizeof(a));
        // A stale ACK — one answering a frame from a previous wake that the
        // air delivered late — must not end this wake's window early and must
        // not set the clock from an older reading.
        if (a.ackSeq != s_wantSeq) return;
        memcpy(&s_ack, &a, sizeof(a));
        s_haveAck = true;
        xSemaphoreGive(s_replySem);
        return;
    }

    if (type == EN_MSG_CFG) {
        if (!s_wantCfg || memcmp(mac, s_cfgFrom, 6) != 0) return;
        CfgChunkMsg c;
        memset(&c, 0, sizeof(c));
        memcpy(&c, data, (size_t)len);          // espnowValidate() bounded len
        if (c.nodeId != s_cfgNode) return;
        memcpy(&s_cfg, &c, sizeof(c));
        s_wantCfg = false;                      // one per request
        s_haveCfg = true;
        xSemaphoreGive(s_replySem);
        return;
    }

    if (type == EN_MSG_WELCOME) {
        WelcomeMsg w;
        memcpy(&w, data, sizeof(w));
        // Broadcast, so every node on the channel hears every WELCOME. Two
        // checks decide whether this one is ours, and both are needed: the
        // target field so we ignore somebody else's, and the signature so a
        // stranger cannot hand us a collector.
        if (memcmp(w.target, s_ownMac, 6) != 0) return;
        if (!espnowVerifyTag(key(), (const uint8_t*)&w,
                             EN_WELCOME_SIGNED_LEN, w.tag)) return;
        memcpy(&s_welcome, &w, sizeof(w));
        memcpy(s_collectorMac, mac, 6);
        s_haveWelcome = true;
        xSemaphoreGive(s_replySem);
    }
}

// ---------------------------------------------------------------------------
// Peers
// ---------------------------------------------------------------------------

static bool addPeer(const uint8_t mac[6], bool encrypt) {
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    if (encrypt) memcpy(p.lmk, key(), 16);
    // Channel 0 is "whatever the interface is on". The node sets its channel
    // explicitly before sending, so pinning a number in the peer as well would
    // be a second place to keep in step with the first.
    p.channel = 0;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = encrypt;
    return esp_now_add_peer(&p) == ESP_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool linkBegin(const NodeLink& link) {
    if (!s_replySem) s_replySem = xSemaphoreCreateBinary();
    if (!s_replySem) return false;

    // persistent(false) keeps the core from writing WiFi settings to NVS on
    // every wake. This node never associates and has no credentials to store,
    // and a flash write a minute is how a partition gets worn out.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);

    // No power save. The node is awake for a third of a second and then gone,
    // so there is nothing for modem sleep to save — and WIFI_PS_MIN_MODEM
    // breaks ESP-NOW unicast, which would make this fail in the one way that
    // leaves broadcast pairing working and nothing else.
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_wifi_get_mac(WIFI_IF_STA, s_ownMac);

    if (link.channel)
        esp_wifi_set_channel(link.channel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) return false;
    if (esp_now_set_pmk((const uint8_t*)ESPNOW_PMK) != ESP_OK) return false;

    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);

    addPeer(BCAST, false);
    if (link.nodeId) addPeer(link.collector, true);
    return true;
}

void linkEnd() {
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
}

const uint8_t* linkOwnMac() { return s_ownMac; }

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

LinkResult linkSendData(const NodeLink& link, const uint8_t* frame, int len, uint16_t seq,
                        uint16_t windowMs) {
    LinkResult r{};
    if (!frame || len <= 0 || len > ESPNOW_MAX_FRAME) return r;

    s_sendDone    = false;
    s_sendOk      = false;
    s_haveAck     = false;
    s_wantSeq     = seq;
    xSemaphoreTake(s_replySem, 0);      // drain anything left from a previous call

    const uint32_t t0 = millis();
    if (esp_now_send(link.collector, frame, (size_t)len) != ESP_OK) {
        r.waitedMs = 0;
        return r;
    }

    // THE WINDOW. A ceiling and not a duration: xSemaphoreTake returns the
    // instant the receive callback gives it, which is normally a few
    // milliseconds. The full window (link.ack_window_ms) is only ever spent
    // on the wakes where no reply is coming — which are exactly the wakes that
    // are about to decide something is wrong.
    const bool got = xSemaphoreTake(s_replySem, pdMS_TO_TICKS(windowMs)) == pdTRUE;
    r.waitedMs = millis() - t0;
    r.sent     = s_sendOk;

    if (got && s_haveAck) {
        r.acked      = true;
        r.epoch      = s_ack.epoch;
        r.intervalS  = s_ack.intervalS;
        r.channel    = s_ack.channel;
        r.rediscover = (s_ack.flags & EN_ACK_REDISCOVER) != 0;
        r.cfgPending = (s_ack.flags & EN_ACK_CFG_PENDING) != 0;
    }
    return r;
}

/// Wait for the send callback of the frame just queued. It fires after the
/// MAC-level ACK (or the last retry), which is a few milliseconds; the bound
/// is only there so a callback that never comes cannot hold the node awake.
static bool waitSent() {
    const uint32_t t0 = millis();
    while (!s_sendDone && (millis() - t0) < 30) delay(1);
    return s_sendDone && s_sendOk;
}

bool linkSendFrame(const NodeLink& link, const void* frame, int len) {
    if (!frame || len <= 0 || len > ESPNOW_MAX_FRAME) return false;
    s_sendDone = false;
    s_sendOk   = false;
    if (esp_now_send(link.collector, (const uint8_t*)frame, (size_t)len) != ESP_OK)
        return false;
    return waitSent();
}

bool linkExchangeCfg(const NodeLink& link, const void* frame, int len, uint16_t windowMs,
                     CfgChunkMsg& out) {
    if (!frame || len <= 0 || len > ESPNOW_MAX_FRAME) return false;
    s_sendDone = false;
    s_sendOk   = false;
    s_haveAck  = false;
    s_haveCfg  = false;
    memcpy(s_cfgFrom, link.collector, 6);
    s_cfgNode  = link.nodeId;
    xSemaphoreTake(s_replySem, 0);
    // Armed BEFORE the send: the collector answers from its receive callback,
    // and a reply can beat esp_now_send() back to this line.
    s_wantCfg  = true;

    if (esp_now_send(link.collector, (const uint8_t*)frame, (size_t)len) != ESP_OK) {
        s_wantCfg = false;
        return false;
    }
    const bool got = xSemaphoreTake(s_replySem, pdMS_TO_TICKS(windowMs)) == pdTRUE;
    s_wantCfg = false;
    if (!got || !s_haveCfg) return false;
    memcpy(&out, &s_cfg, sizeof(out));
    return true;
}

// ---------------------------------------------------------------------------
// Pairing
// ---------------------------------------------------------------------------

bool linkPair(NodeLink& io, uint32_t* epochOut) {
    // A nonce per attempt so two sweeps are not byte-identical on the air.
    // esp_random() is the hardware RNG; it is seeded without WiFi being
    // associated, which matters because this runs before the node has a
    // collector at all.
    for (uint8_t ch = 1; ch <= NODE_MAX_CHANNEL; ch++) {
        if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;

        DiscoverMsg d;
        if (!espnowFillDiscover(d, key(), io.nodeId,
                                s_ownMac, esp_random()))
            return false;

        s_haveWelcome = false;
        xSemaphoreTake(s_replySem, 0);
        if (esp_now_send(BCAST, (const uint8_t*)&d, sizeof(d)) != ESP_OK) continue;

        if (xSemaphoreTake(s_replySem, pdMS_TO_TICKS(NODE_PAIR_DWELL_MS)) != pdTRUE)
            continue;
        if (!s_haveWelcome) continue;

        if (epochOut) *epochOut = s_welcome.epoch;
        io.nodeId    = s_welcome.nodeId;
        io.channel   = s_welcome.channel ? s_welcome.channel : ch;
        io.intervalS = s_welcome.intervalS ? s_welcome.intervalS
                                           : (uint16_t)NODE_INTERVAL_S;
        memcpy(io.collector, s_collectorMac, 6);
        memcpy(io.bssid, s_welcome.bssid, 6);
        strncpy(io.ssid, s_welcome.ssid, sizeof(io.ssid) - 1);
        io.ssid[sizeof(io.ssid) - 1] = '\0';

        // The WELCOME arrived in the clear; everything after it is encrypted,
        // which is what adding the peer with the key does.
        addPeer(io.collector, true);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Finding a moved channel
// ---------------------------------------------------------------------------

ScanResult linkFindChannel(const NodeLink& link, const char* nextSsid) {
    ScanResult out{};
    // Passive: listen for beacons rather than probe for them. It is slower per
    // channel but it transmits nothing, and a node doing this because it has
    // lost its collector has no business shouting on thirteen channels.
    const int n = WiFi.scanNetworks(false, true, true, 300);
    if (n <= 0) {
        WiFi.scanDelete();
        return out;
    }

    const bool wantNext = nextSsid && nextSsid[0];
    uint8_t byBssid = 0, bySsid = 0;
    int32_t nextRssi = -1000;
    for (int i = 0; i < n; i++) {
        const uint8_t* b = WiFi.BSSID(i);
        const String ssid = WiFi.SSID(i);
        if (!byBssid && b && memcmp(b, link.bssid, 6) == 0) byBssid = (uint8_t)WiFi.channel(i);
        if (!bySsid && link.ssid[0] && ssid == link.ssid)
            bySsid = (uint8_t)WiFi.channel(i);
        if (wantNext && ssid == nextSsid && WiFi.RSSI(i) > nextRssi) {
            nextRssi  = WiFi.RSSI(i);
            out.nextCh = (uint8_t)WiFi.channel(i);
            if (b) memcpy(out.nextBssid, b, 6);
        }
    }
    WiFi.scanDelete();

    // BSSID first because it is exact. SSID is the fallback and not the
    // primary: on a mesh it will match several radios and the first one found
    // is not necessarily the one the collector is on — but a wrong guess
    // costs one wake, and having no fallback costs every wake until somebody
    // notices.
    out.storedCh = byBssid ? byBssid : bySsid;
    return out;
}
