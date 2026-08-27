// ============================================================================
// src/espnow/EspNowProto.h
//
// The wire format between a battery node and the collector, over ESP-NOW.
//
// Both sides compile THIS file. That is the whole point of it being a header
// with no dependencies: the node firmware in node_espnow/ and the collector's
// src/espnow/EspNowIngest.cpp cannot drift apart, because a change here is a
// change to both or it does not build.
//
// WHY BINARY AND NOT JSON
// -----------------------
// ESP-NOW carries at most 250 bytes of payload per frame, and that is a hard
// MAC-layer limit, not a buffer we can raise. The JSON the ESP8266 node posts
// to /api/ingest measures 237 bytes for a BME280 plus battery — under the cap
// with 13 bytes to spare, which sounds fine until an ingest token is added and
// it becomes 264 and does not fit at all. A format that is one field away from
// not fitting is not a format, so this is packed binary: 12 bytes of header
// and 12 per sample.
//
// The saving is not the point either. Fitting with room for fifteen samples in
// one frame is the point, because that is what lets a node that could not
// reach the collector keep its readings and send them later.
//
// ENDIANNESS AND PACKING
// ----------------------
// Both ends are little-endian Xtensa/RISC-V, so the multi-byte fields go on
// the wire in native order and there is no byte swapping anywhere. The
// static_asserts at the bottom of this file are what make that a checked claim
// rather than an assumption: they fail the build on any host where the layout
// differs, including the x86-64 CI box that runs the host tests.
//
// AUTHENTICITY
// ------------
// DATA and ACK travel between peers added with the shared LMK, so the radio
// encrypts and authenticates them (CCMP) and nothing here re-does that work.
//
// The two handshake messages cannot use that, and the reason is a bootstrap
// nobody escapes: ESP-NOW decrypts an incoming frame only from a peer already
// added with the key, so before the two ends know each other's MAC addresses
// neither can receive anything encrypted from the other. DISCOVER and WELCOME
// are therefore both broadcast, both in the clear, and both carry a truncated
// HMAC over their own bytes instead. See the comments on each for what that
// does and does not buy.
//
// A NOTE ON REPLAY
// ----------------
// Every DATA frame carries a sequence number, and the collector rejects one
// that does not advance. That is a cheap guard against a captured frame being
// re-sent, not a strong one: a node that resets starts its sequence over, so
// FLAG_FIRST_BOOT has to be honoured, and honouring it is the hole. It is
// worth having anyway — it also catches duplicates from the radio's own
// retries, which is the case that actually happens.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Framing constants
// ---------------------------------------------------------------------------

/// First byte of every message. Not a checksum — just enough to drop a frame
/// from some other ESP-NOW device sharing the channel before we parse it.
static const uint8_t ESPNOW_MAGIC = 0xE5;

/// Bumped when a field changes meaning. The collector rejects anything else,
/// rather than guessing: a node running old firmware should go silent and be
/// noticed, not report plausible nonsense.
static const uint8_t ESPNOW_PROTO_VER = 1;

/// The MAC-layer payload limit. Not ours to raise.
static const int ESPNOW_MAX_FRAME = 250;

/// How many samples one DATA frame can carry. 15 × 12 + 12 = 192 bytes, which
/// leaves headroom under ESPNOW_MAX_FRAME rather than sitting flush against it.
static const uint8_t ESPNOW_MAX_SAMPLES = 15;

enum EspNowMsgType : uint8_t {
    EN_MSG_DATA     = 1,   ///< node → collector, encrypted, unicast
    EN_MSG_ACK      = 2,   ///< collector → node, encrypted, unicast
    EN_MSG_DISCOVER = 3,   ///< node → broadcast, plaintext + HMAC
    EN_MSG_WELCOME  = 4,   ///< collector → broadcast, plaintext + HMAC
};

// ---------------------------------------------------------------------------
// Absent-value sentinels
// ---------------------------------------------------------------------------
// A BMP280 has no humidity sensor and a node without a divider fitted has no
// battery voltage. Sending a zero for either would be a lie the collector
// could not tell from a real reading, so each field reserves one value to mean
// "not measured" and the pack/unpack helpers below map it to NaN.
static const uint16_t EN_ABSENT_U16 = 0xFFFF;
static const uint32_t EN_ABSENT_U32 = 0xFFFFFFFFu;
static const int16_t  EN_ABSENT_I16 = (int16_t)0x8000;

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

/// One measurement instant. 12 bytes.
struct __attribute__((packed)) EnvSample {
    /// Seconds before DataMsg::epoch that this sample was taken. 0 for the
    /// live one. This is what lets a buffered burst carry honest timestamps
    /// without spending four bytes per sample on an absolute epoch.
    uint16_t dt_s;
    int16_t  t_c100;    ///< °C × 100.  EN_ABSENT_I16 if absent.
    uint16_t rh_x100;   ///< %RH × 100. EN_ABSENT_U16 if absent (BMP280).
    uint32_t press_pa;  ///< Pa.        EN_ABSENT_U32 if absent.
    uint16_t vbat_mv;   ///< Battery mV at the divider. EN_ABSENT_U16 if absent.
};

enum EspNowDataFlags : uint8_t {
    /// Set on the first frame after a reset, when `seq` has restarted from
    /// zero. Without it the collector's replay guard would reject every frame
    /// from a node that rebooted until the counter caught up.
    EN_FLAG_FIRST_BOOT = 1 << 0,
    /// The node is asking to be re-provisioned — it lost its stored channel or
    /// the collector's MAC. The collector answers with WELCOME instead of ACK.
    EN_FLAG_WANT_CONFIG = 1 << 1,
};

/// node → collector. 12-byte header, then `count` samples.
struct __attribute__((packed)) DataMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_DATA
    uint8_t  nodeId;    ///< 1..254, assigned by the collector at provisioning
    uint16_t seq;       ///< advances by one per frame; wraps
    uint8_t  count;     ///< 1..ESPNOW_MAX_SAMPLES
    uint8_t  flags;     ///< EspNowDataFlags
    /// The node's idea of the wall clock, from the last ACK it received.
    /// 0 means it has never been told, and the collector should stamp on
    /// arrival instead of trusting this.
    uint32_t epoch;
    EnvSample s[ESPNOW_MAX_SAMPLES];
};

/// Size of a DATA frame carrying `count` samples — what goes on the wire, as
/// opposed to sizeof(DataMsg), which is always the fifteen-sample maximum.
static inline int espnowDataLen(uint8_t count) {
    return (int)(sizeof(DataMsg) - sizeof(EnvSample) * (size_t)(ESPNOW_MAX_SAMPLES - count));
}

enum EspNowAckFlags : uint8_t {
    /// The collector does not recognise this node and wants it to re-run
    /// discovery.
    ///
    /// NARROWER THAN IT LOOKS, and worth knowing why before relying on it. A
    /// collector that lost its peer table cannot decrypt this node's DATA at
    /// all — the radio drops the frame before any callback runs — so it never
    /// gets the chance to answer. This flag is reachable only when the peer
    /// entry survived but the node record did not, which is a narrow case.
    ///
    /// Recovery from a reflashed collector is the node's job instead: enough
    /// unanswered wakes and it runs the pairing sweep again. See
    /// docs/ESPNOW_NODE.md.
    EN_ACK_REDISCOVER = 1 << 0,
};

/// collector → node, sent immediately from the receive path.
///
/// This is the reply the node stays awake for. Three things ride on it and
/// each is worth the milliseconds:
///
///   epoch      the node has no RTC crystal and no NTP; this is its only
///              source of wall-clock time, and it needs one to timestamp a
///              buffered burst.
///   intervalS  lets the wake period be changed from the collector's web UI
///              without reflashing a node that may be behind a wall.
///   silence    the absence of this frame is the node's cue to look for a
///              moved channel. See docs/ESPNOW_NODE.md for why that trigger
///              has to live on the node and cannot live on the collector.
struct __attribute__((packed)) AckMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_ACK
    uint8_t  nodeId;
    uint16_t ackSeq;    ///< echoes DataMsg::seq, so a stale ACK is ignorable
    uint8_t  channel;   ///< the channel the collector is on as it replies
    uint8_t  flags;     ///< EspNowAckFlags
    uint32_t epoch;     ///< collector's wall clock, 0 if it has none either
    uint16_t intervalS; ///< desired seconds between wakes; 0 = keep yours
};

/// node → broadcast, plaintext. Sent while sweeping channels to find a
/// collector it has not been provisioned to yet.
///
/// WHAT THE TAG IS FOR
/// -------------------
/// Broadcast frames cannot be encrypted, so this one is signed instead: `tag`
/// is the first 8 bytes of HMAC-SHA256(LMK, everything before `tag`). That
/// proves the sender knows the shared key, which is what stops a stranger's
/// node from being adopted by walking past the house during a pairing window.
///
/// It is deliberately not more than that. The frame is readable by anyone in
/// range — it leaks a MAC address and a node number — and `nonce` makes each
/// one distinct without making a captured frame useless to replay. The real
/// limit on replay here is that the collector only listens for DISCOVER while
/// a pairing window is open, which a human has to press a button to start.
struct __attribute__((packed)) DiscoverMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_DISCOVER
    uint8_t  nodeId;    ///< 0 if never provisioned, else the id it remembers
    uint8_t  mac[6];    ///< the node's STA MAC, so the reply can be unicast
    uint32_t nonce;
    uint8_t  tag[8];    ///< truncated HMAC-SHA256 over the 14 bytes above
};

/// Offset and length of the region DiscoverMsg::tag authenticates.
static const size_t EN_DISCOVER_SIGNED_LEN = 14;

/// collector → node, in answer to DISCOVER.
///
/// BROADCAST AND SIGNED, NOT UNICAST AND ENCRYPTED, AND THAT IS FORCED.
/// ESP-NOW decrypts an incoming frame only from a peer already added with the
/// key — so for the node to receive an encrypted WELCOME it would have to have
/// added the collector as a peer already, which means knowing the collector's
/// MAC address, which is what the WELCOME is for. There is no ordering that
/// resolves that.
///
/// So the reply goes out unencrypted to the broadcast address, carries the MAC
/// it is meant for, and is authenticated the same way DISCOVER is: an HMAC
/// over everything before the tag. A node ignores a WELCOME addressed to
/// somebody else, and one whose tag does not verify.
///
/// What that discloses to anyone in range is an SSID and a BSSID — both of
/// which the access point itself broadcasts continuously — plus a node number
/// and a wake interval. Nothing that was private a moment earlier.
struct __attribute__((packed)) WelcomeMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;       ///< EN_MSG_WELCOME
    uint8_t  nodeId;     ///< the id the collector has assigned; node stores it
    uint8_t  channel;    ///< where the collector is now
    uint8_t  reserved;
    uint16_t intervalS;
    uint32_t epoch;
    /// The access point to look for when the channel moves. BSSID is exact and
    /// SSID is the fallback, because a mesh or a repeater changes the BSSID
    /// under you while the SSID stays put. The node keeps both and prefers the
    /// BSSID when it still matches something on the air.
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  target[6];  ///< the node this is for; everyone else ignores it
    uint8_t  tag[8];     ///< truncated HMAC-SHA256 over the 57 bytes above
};

/// Offset and length of the region WelcomeMsg::tag authenticates.
static const size_t EN_WELCOME_SIGNED_LEN = 57;

// ---------------------------------------------------------------------------
// Field packing
// ---------------------------------------------------------------------------
// Free functions rather than methods, so a caller can pack a value it holds
// loose without building a struct first, and so the host test can exercise the
// sentinel handling in isolation. `float` here is only ever a C float; nothing
// in this header uses <math.h>, so the NaN is built rather than named.

static inline float enNaN() {
    // 0x7FC00000 is a quiet NaN. Built by bits because -ffast-math on some
    // toolchains folds NAN comparisons away, and the sentinel round-trip is
    // exactly what the host test needs to be able to see.
    uint32_t bits = 0x7FC00000u;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static inline bool enIsAbsent(float v) { return !(v == v); }  // NaN != itself

static inline int16_t enPackTemp(float c) {
    if (enIsAbsent(c) || c < -320.0f || c > 320.0f) return EN_ABSENT_I16;
    return (int16_t)(c * 100.0f + (c >= 0 ? 0.5f : -0.5f));
}
static inline float enUnpackTemp(int16_t v) {
    return (v == EN_ABSENT_I16) ? enNaN() : (float)v / 100.0f;
}

static inline uint16_t enPackRh(float rh) {
    if (enIsAbsent(rh) || rh < 0.0f || rh > 100.0f) return EN_ABSENT_U16;
    return (uint16_t)(rh * 100.0f + 0.5f);
}
static inline float enUnpackRh(uint16_t v) {
    return (v == EN_ABSENT_U16) ? enNaN() : (float)v / 100.0f;
}

static inline uint32_t enPackPress(float pa) {
    // Below 15 kPa is above the altitude any of this is meant for, and above
    // 200 kPa is not weather. Out of range is stored as absent, not clamped:
    // a clamped value reads as a measurement and this is not one.
    if (enIsAbsent(pa) || pa < 15000.0f || pa > 200000.0f) return EN_ABSENT_U32;
    return (uint32_t)(pa + 0.5f);
}
static inline float enUnpackPress(uint32_t v) {
    return (v == EN_ABSENT_U32) ? enNaN() : (float)v;
}

static inline uint16_t enPackMv(float mv) {
    if (enIsAbsent(mv) || mv < 0.0f || mv > 65000.0f) return EN_ABSENT_U16;
    return (uint16_t)(mv + 0.5f);
}
static inline float enUnpackMv(uint16_t v) {
    return (v == EN_ABSENT_U16) ? enNaN() : (float)v;
}

/// Zero a sample to "nothing measured" — every field its own absent sentinel.
/// Callers fill in what they have; whatever they skip stays honestly empty.
static inline void enClearSample(EnvSample& s) {
    s.dt_s     = 0;
    s.t_c100   = EN_ABSENT_I16;
    s.rh_x100  = EN_ABSENT_U16;
    s.press_pa = EN_ABSENT_U32;
    s.vbat_mv  = EN_ABSENT_U16;
}

// ---------------------------------------------------------------------------
// Frame validation
// ---------------------------------------------------------------------------

/// True if `buf` is one of ours and long enough to be the message it claims.
///
/// Every receive path starts here, and it is the only place that decides a
/// frame is worth looking at. `type` is written only on success.
///
/// The length check is per-type and exact where it can be: a DATA frame must
/// be precisely the length its own `count` implies, which means a truncated
/// burst is rejected rather than parsed into whatever follows it in the
/// receive buffer.
static inline bool espnowValidate(const uint8_t* buf, int len, uint8_t& type) {
    if (!buf || len < 4) return false;
    if (buf[0] != ESPNOW_MAGIC) return false;
    if (buf[1] != ESPNOW_PROTO_VER) return false;

    const uint8_t t = buf[2];
    switch (t) {
        case EN_MSG_DATA: {
            if (len < (int)sizeof(DataMsg) - (int)sizeof(EnvSample) * (ESPNOW_MAX_SAMPLES - 1))
                return false;                       // shorter than a 1-sample frame
            const uint8_t count = buf[6];
            if (count < 1 || count > ESPNOW_MAX_SAMPLES) return false;
            if (len != espnowDataLen(count)) return false;
            break;
        }
        case EN_MSG_ACK:
            if (len != (int)sizeof(AckMsg)) return false;
            break;
        case EN_MSG_DISCOVER:
            if (len != (int)sizeof(DiscoverMsg)) return false;
            break;
        case EN_MSG_WELCOME:
            if (len != (int)sizeof(WelcomeMsg)) return false;
            break;
        default:
            return false;
    }
    type = t;
    return true;
}

/// Copy a validated DATA frame into `out`, zero-filling the unused samples.
///
/// The caller must have run espnowValidate() first — this does not re-check,
/// it copies. Splitting them keeps the validation in one place and lets the
/// receive path decide what to do with each type before it commits to a copy.
static inline void espnowDecodeData(const uint8_t* buf, int len, DataMsg& out) {
    memset(&out, 0, sizeof(out));
    memcpy(&out, buf, (size_t)len);
}

/// Serialise `msg` into `buf`, returning the byte count or -1 if it will not
/// fit. Only `msg.count` samples are written, which is what keeps a one-sample
/// frame 24 bytes and not 192.
static inline int espnowEncodeData(const DataMsg& msg, uint8_t* buf, size_t cap) {
    if (msg.count < 1 || msg.count > ESPNOW_MAX_SAMPLES) return -1;
    const int n = espnowDataLen(msg.count);
    if (!buf || cap < (size_t)n) return -1;
    memcpy(buf, &msg, (size_t)n);
    return n;
}

// ---------------------------------------------------------------------------
// Layout assertions
// ---------------------------------------------------------------------------
// A packed struct whose size drifts is a protocol break that compiles. These
// run on the host test box and on both targets, so a member added in the wrong
// place stops the build on whichever is compiled first.
static_assert(sizeof(EnvSample)   == 12,  "EnvSample must stay 12 bytes");
static_assert(sizeof(DataMsg)     == 192, "DataMsg header is 12 + 15*12");
static_assert(sizeof(AckMsg)      == 14,  "AckMsg must stay 14 bytes");
static_assert(sizeof(DiscoverMsg) == 22,  "DiscoverMsg must stay 22 bytes");
static_assert(sizeof(WelcomeMsg)  == 65,  "WelcomeMsg must stay 65 bytes");
static_assert(sizeof(DataMsg) <= ESPNOW_MAX_FRAME, "a full burst must fit one frame");
static_assert(sizeof(WelcomeMsg) <= ESPNOW_MAX_FRAME, "WELCOME must fit one frame");
static_assert(offsetof(DataMsg, count) == 6, "espnowValidate() reads count at [6]");
static_assert(offsetof(DiscoverMsg, tag) == EN_DISCOVER_SIGNED_LEN,
              "the HMAC must cover exactly the bytes before the tag");
static_assert(offsetof(WelcomeMsg, tag) == EN_WELCOME_SIGNED_LEN,
              "the HMAC must cover exactly the bytes before the tag");
