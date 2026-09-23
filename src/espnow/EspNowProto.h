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

    // Remote configuration and the dynamic-sensor data frame —
    // docs/NODE_CONFIG.md §5. All five are encrypted unicast between peers
    // that already paired, so like DATA and ACK they carry no tag of their
    // own. New TYPES rather than a new ESPNOW_PROTO_VER, on purpose: a
    // collector that predates them drops an unknown type in espnowValidate()
    // and keeps talking to the node in the old types, and an old node never
    // sends them. Bumping the version would have made every old node on a new
    // collector go silent at once, which is the opposite of the upgrade story.
    EN_MSG_CFG_GET    = 5,   ///< node → collector: "send me the config from offset N"
    EN_MSG_CFG        = 6,   ///< collector → node: one slice of the desired config
    EN_MSG_CFG_ACK    = 7,   ///< node → collector: applied, or rejected and why
    EN_MSG_CFG_REPORT = 8,   ///< node → collector: one slice of the node's own config
    EN_MSG_DATA2      = 9,   ///< node → collector: readings as (metric id, index, value)
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

    /// The collector holds a newer config for this node than the node last
    /// applied (desired.rev > applied_rev) — docs/NODE_CONFIG.md §5.
    ///
    /// A FLAG ON THE ACK, NOT A PUSH. The node is asleep except for the few
    /// milliseconds after its own DATA frame, so the collector has nowhere to
    /// send a config to; the ACK is the one frame the node is guaranteed to be
    /// listening for. Seeing this bit, the node stays awake a little longer and
    /// pulls the config with CFG_GET. A node built before this flag existed
    /// ignores the bit, which is exactly the right thing for it to do.
    EN_ACK_CFG_PENDING = 1 << 1,
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
// Remote configuration — docs/NODE_CONFIG.md §5
// ---------------------------------------------------------------------------
// The config itself is the §1 JSON document (src/nodecfg/NodeConfigJson.h),
// compact, without `net` and without the key. It is JSON on the radio for
// the same reason it is JSON everywhere else: one codec, one validator, and
// a collector that can store what the node reported without translating it.
// It does not fit one frame, so it travels in slices that the receiver
// reassembles — see EnCfgAssembler below.

/// Most document bytes one CFG / CFG_REPORT frame carries. 11 bytes of header
/// plus 200 is 211, well under ESPNOW_MAX_FRAME; the round number is what
/// keeps the slice arithmetic obvious in a log.
static const uint8_t  EN_CFG_CHUNK_MAX = 200;

/// Largest document either side will send or reassemble. A full ESP-NOW
/// config — eight sensor entries with every field — measures well under this
/// (the host test builds one and checks), and a hard ceiling is what lets the
/// receiver hold the reassembly in a fixed buffer.
static const uint16_t EN_CFG_MAX_TOTAL = 1024;

/// CFG_ACK's field path and reason, terminator included. The validator's
/// Issue struct uses the same sizes (static_asserted there), so a rejection
/// reaches the collector's UI exactly as the node's validator worded it.
static const size_t EN_CFG_FIELD_LEN  = 24;
static const size_t EN_CFG_REASON_LEN = 48;

enum EspNowCfgStatus : uint8_t {
    EN_CFG_OK       = 0,
    EN_CFG_REJECTED = 1,
};

/// node → collector: request the desired config starting at `offset`.
///
/// The node asks rather than the collector pushing, and asks for one slice at
/// a time, because the node decides how long it stays awake: a wake that runs
/// out of time simply stops asking, and the next one starts again from 0.
/// `haveRev` is the rev the node is running, so the collector can tell a node
/// that is behind from one asking for a config it already has.
struct __attribute__((packed)) CfgGetMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_CFG_GET
    uint8_t  nodeId;
    uint16_t haveRev;
    uint16_t offset;
};

/// CFG (collector → node) and CFG_REPORT (node → collector): one slice of a
/// config document. On the wire only `len` bytes of `data` are sent.
///
/// `total == 0` is a message of its own: in a CFG it is the collector's reply
/// to the last slice of a CFG_REPORT, and means "your local config is now rev
/// `rev`" — the node clears `local` and adopts the number. It carries no data.
struct __attribute__((packed)) CfgChunkMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_CFG or EN_MSG_CFG_REPORT
    uint8_t  nodeId;
    uint16_t rev;       ///< CFG: the desired rev. CFG_REPORT: the node's rev.
    uint16_t total;     ///< whole document length, 0..EN_CFG_MAX_TOTAL
    uint16_t offset;    ///< where this slice starts
    uint8_t  len;       ///< bytes of data in this frame, 0..EN_CFG_CHUNK_MAX
    uint8_t  data[EN_CFG_CHUNK_MAX];
};

/// Header bytes of a CfgChunkMsg — everything before `data`.
static const int EN_CFG_CHUNK_HDR = 11;

/// Wire length of a CFG / CFG_REPORT frame carrying `len` data bytes.
static inline int espnowCfgChunkLen(uint8_t len) {
    return EN_CFG_CHUNK_HDR + (int)len;
}

/// node → collector: the outcome of applying config `rev`.
///
/// `field` and `reason` are NUL-terminated inside their arrays and empty on
/// success. A rejected config leaves the node on its previous one (§0.3), so
/// this frame is how the collector's UI learns to say "rejected: GPIO6 is the
/// flash bus" instead of "pending" forever.
struct __attribute__((packed)) CfgAckMsg {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_CFG_ACK
    uint8_t  nodeId;
    uint16_t rev;
    uint8_t  status;    ///< EspNowCfgStatus
    char     field[EN_CFG_FIELD_LEN];
    char     reason[EN_CFG_REASON_LEN];
};

// ---------------------------------------------------------------------------
// DATA2 — the dynamic-sensor data frame, docs/NODE_CONFIG.md §5
// ---------------------------------------------------------------------------
// DATA is four fixed fields per sample because the node used to have exactly
// one sensor. A configurable node can have any eight metrics, so DATA2 says
// which metric each value is: a one-byte catalogue id (src/nodecfg/
// MetricCatalog.h), a one-byte index (the DS18B20 probe number), and the value
// as a plain float32. Six bytes a value instead of two or four — the price of
// not needing a new frame type for every sensor anyone adds.
//
// Layout: the 12-byte DATA header with type = 9 and count = samples, then per
// sample `dt_s u16 | n u8 | n × {metric u8, index u8, value f32}`. Samples are
// variable length, so there is no struct for the whole frame; the helpers
// below build and walk it, and espnowValidate() walks it once, exactly, before
// anyone else reads it.

/// Most values in one DATA2 sample: the eight-metric budget plus
/// battery_voltage. MetricCatalog.h static_asserts that this is enough.
static const uint8_t EN_DATA2_MAX_VALUES = 9;

/// The 12-byte header, identical in layout to the front of DataMsg.
struct __attribute__((packed)) Data2Header {
    uint8_t  magic;
    uint8_t  ver;
    uint8_t  type;      ///< EN_MSG_DATA2
    uint8_t  nodeId;
    uint16_t seq;       ///< shares the node's DATA sequence counter
    uint8_t  count;     ///< samples in the frame, >= 1
    uint8_t  flags;     ///< EspNowDataFlags
    uint32_t epoch;     ///< as DataMsg::epoch
};

/// One value on the wire. `value` is never NaN from a well-behaved node — an
/// absent reading is simply not sent — but the collector must still drop a
/// non-finite one rather than store it.
struct __attribute__((packed)) Data2Value {
    uint8_t metric;     ///< MetricId
    uint8_t index;      ///< probe ordinal for probe_temp, else 0
    float   value;
};

/// A decoded sample (the reader's output; not a wire layout).
struct Data2Sample {
    uint16_t   dt_s;
    uint8_t    n;
    Data2Value v[EN_DATA2_MAX_VALUES];
};

static const int EN_DATA2_HDR = 12;

/// Wire length of one sample with `n` values.
static inline int espnowData2SampleLen(uint8_t n) {
    return 3 + (int)n * (int)sizeof(Data2Value);
}

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

/// Little-endian u16 at `p`, without an aligned load. The receive buffer is
/// whatever the radio driver handed over, and a packed-struct member read
/// through a cast pointer is a misaligned access on a strict target.
static inline uint16_t enRdU16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/// Walk a DATA2 frame's samples once, checking every length on the way.
///
/// This is what makes the variable-length format safe to read afterwards: it
/// consumes exactly `count` samples, refuses any sample that claims more than
/// EN_DATA2_MAX_VALUES values or runs past `len`, and refuses a frame with
/// bytes left over. After it returns true, espnowData2Next() cannot read out
/// of bounds, and does not check again.
static inline bool espnowData2Walk(const uint8_t* buf, int len) {
    if (!buf || len < EN_DATA2_HDR + 3 || len > ESPNOW_MAX_FRAME) return false;
    const uint8_t count = buf[6];
    if (count < 1) return false;
    int pos = EN_DATA2_HDR;
    for (uint8_t i = 0; i < count; i++) {
        if (pos + 3 > len) return false;
        const uint8_t n = buf[pos + 2];
        if (n > EN_DATA2_MAX_VALUES) return false;
        pos += espnowData2SampleLen(n);
        if (pos > len) return false;
    }
    return pos == len;
}

/// True if a CFG / CFG_REPORT frame's own numbers agree with each other and
/// with its length: `len` data bytes follow the header exactly, the slice lies
/// inside `total`, and `total` is under the ceiling. A `total == 0` frame (the
/// "adopted rev" reply) must carry nothing.
static inline bool espnowCfgChunkOk(const uint8_t* buf, int len) {
    if (len < EN_CFG_CHUNK_HDR) return false;
    const uint16_t total  = enRdU16(buf + 6);
    const uint16_t offset = enRdU16(buf + 8);
    const uint8_t  dlen   = buf[10];
    if (dlen > EN_CFG_CHUNK_MAX) return false;
    if (len != espnowCfgChunkLen(dlen)) return false;
    if (total > EN_CFG_MAX_TOTAL) return false;
    if (total == 0) return offset == 0 && dlen == 0;
    // A slice may be empty only if it is the end: an empty slice in the middle
    // would be a request loop that never advances.
    if (dlen == 0) return false;
    return (uint32_t)offset + dlen <= total;
}

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
        case EN_MSG_CFG_GET:
            if (len != (int)sizeof(CfgGetMsg)) return false;
            if (enRdU16(buf + 6) >= EN_CFG_MAX_TOTAL) return false;
            break;
        case EN_MSG_CFG:
        case EN_MSG_CFG_REPORT:
            if (!espnowCfgChunkOk(buf, len)) return false;
            break;
        case EN_MSG_CFG_ACK:
            if (len != (int)sizeof(CfgAckMsg)) return false;
            if (buf[6] > EN_CFG_REJECTED) return false;
            // Both strings are read as C strings by the collector (logged,
            // stored, put in JSON), so a frame without a terminator inside
            // each array would be a read past its end. Refuse it here.
            if (!memchr(buf + 7, 0, EN_CFG_FIELD_LEN)) return false;
            if (!memchr(buf + 7 + EN_CFG_FIELD_LEN, 0, EN_CFG_REASON_LEN)) return false;
            break;
        case EN_MSG_DATA2:
            if (!espnowData2Walk(buf, len)) return false;
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
// DATA2 build and read
// ---------------------------------------------------------------------------
// The node builds a frame by appending whole samples until the next one does
// not fit; the collector reads one back sample by sample. Neither side ever
// indexes the buffer itself — the offsets live here, next to the validator
// that proves them, and nowhere else.

/// Start a DATA2 frame in `buf`: writes the 12-byte header with count 0 and
/// returns the length so far (12), or -1 if `cap` cannot hold even that.
/// A frame is not valid until at least one sample has been appended.
static inline int espnowData2Begin(uint8_t* buf, size_t cap, uint8_t nodeId,
                                   uint16_t seq, uint8_t flags, uint32_t epoch) {
    if (!buf || cap < (size_t)EN_DATA2_HDR) return -1;
    Data2Header h;
    h.magic  = ESPNOW_MAGIC;
    h.ver    = ESPNOW_PROTO_VER;
    h.type   = EN_MSG_DATA2;
    h.nodeId = nodeId;
    h.seq    = seq;
    h.count  = 0;
    h.flags  = flags;
    h.epoch  = epoch;
    memcpy(buf, &h, sizeof(h));
    return EN_DATA2_HDR;
}

/// Append one sample of `n` values to a frame begun with espnowData2Begin().
///
/// Whole samples or nothing: when this sample would take the frame past
/// `cap` or past ESPNOW_MAX_FRAME it returns false and leaves the frame as it
/// was, so the caller sends what it has and keeps the rest for the next frame.
/// `len` is the frame length so far and is advanced on success.
static inline bool espnowData2Append(uint8_t* buf, size_t cap, int& len, uint16_t dt_s,
                                     const Data2Value* v, uint8_t n) {
    if (!buf || len < EN_DATA2_HDR || n > EN_DATA2_MAX_VALUES) return false;
    if (n > 0 && !v) return false;
    if (buf[6] == 0xFF) return false;                 // count would wrap
    const int add = espnowData2SampleLen(n);
    const int limit = cap < (size_t)ESPNOW_MAX_FRAME ? (int)cap : ESPNOW_MAX_FRAME;
    if (len + add > limit) return false;
    uint8_t* p = buf + len;
    memcpy(p, &dt_s, 2);
    p[2] = n;
    if (n) memcpy(p + 3, v, (size_t)n * sizeof(Data2Value));
    len += add;
    buf[6]++;
    return true;
}

/// Read position inside a validated DATA2 frame.
struct Data2Cursor {
    const uint8_t* buf;
    int            len;
    int            pos;
    uint8_t        left;   ///< samples not yet read
};

/// Open a frame espnowValidate() has accepted as EN_MSG_DATA2: copy out the
/// header and position `cur` at the first sample. Does not re-validate — the
/// same split as espnowDecodeData(), for the same reason.
static inline void espnowData2Open(const uint8_t* buf, int len, Data2Header& hdr,
                                   Data2Cursor& cur) {
    memcpy(&hdr, buf, sizeof(hdr));
    cur.buf  = buf;
    cur.len  = len;
    cur.pos  = EN_DATA2_HDR;
    cur.left = hdr.count;
}

/// The next sample, oldest first in the order the node appended them.
/// False when there are no more.
static inline bool espnowData2Next(Data2Cursor& cur, Data2Sample& out) {
    if (cur.left == 0 || cur.pos + 3 > cur.len) return false;
    const uint8_t* p = cur.buf + cur.pos;
    memcpy(&out.dt_s, p, 2);
    out.n = p[2];
    if (out.n > EN_DATA2_MAX_VALUES) return false;       // unreachable after validate
    const int step = espnowData2SampleLen(out.n);
    if (cur.pos + step > cur.len) return false;          // likewise
    if (out.n) memcpy(out.v, p + 3, (size_t)out.n * sizeof(Data2Value));
    cur.pos += step;
    cur.left--;
    return true;
}

// ---------------------------------------------------------------------------
// Config slices, acknowledgements and requests
// ---------------------------------------------------------------------------

static inline void enFillHeader(uint8_t* m, uint8_t type, uint8_t nodeId) {
    m[0] = ESPNOW_MAGIC;
    m[1] = ESPNOW_PROTO_VER;
    m[2] = type;
    m[3] = nodeId;
}

/// Build a CFG_GET asking for the slice at `offset`.
static inline void espnowFillCfgGet(CfgGetMsg& m, uint8_t nodeId, uint16_t haveRev,
                                    uint16_t offset) {
    enFillHeader((uint8_t*)&m, EN_MSG_CFG_GET, nodeId);
    m.haveRev = haveRev;
    m.offset  = offset;
}

/// Build the CFG or CFG_REPORT frame that carries `doc[offset…]` — at most
/// EN_CFG_CHUNK_MAX bytes of it. Returns the wire length, or -1 when the
/// document is too big for the protocol or `offset` is not inside it.
///
/// `doc` is the compact JSON the codec wrote; it need not be NUL-terminated,
/// `total` is its length. Passing total = 0 (and doc = nullptr) builds the
/// data-less "your local config is now rev N" reply.
static inline int espnowFillCfgChunk(CfgChunkMsg& m, uint8_t type, uint8_t nodeId,
                                     uint16_t rev, const char* doc, uint16_t total,
                                     uint16_t offset) {
    if (type != EN_MSG_CFG && type != EN_MSG_CFG_REPORT) return -1;
    if (total > EN_CFG_MAX_TOTAL) return -1;
    if (total == 0) {
        if (offset != 0) return -1;
    } else if (!doc || offset >= total) {
        return -1;
    }
    enFillHeader((uint8_t*)&m, type, nodeId);
    m.rev    = rev;
    m.total  = total;
    m.offset = offset;
    const uint16_t left = (uint16_t)(total - offset);
    m.len = (uint8_t)(left < EN_CFG_CHUNK_MAX ? left : EN_CFG_CHUNK_MAX);
    if (m.len) memcpy(m.data, doc + offset, m.len);
    return espnowCfgChunkLen(m.len);
}

/// Build a CFG_ACK. `field` and `reason` are cut to fit their arrays and are
/// always terminated; pass nullptr (or "") on success.
static inline void espnowFillCfgAck(CfgAckMsg& m, uint8_t nodeId, uint16_t rev,
                                    uint8_t status, const char* field, const char* reason) {
    memset(&m, 0, sizeof(m));
    enFillHeader((uint8_t*)&m, EN_MSG_CFG_ACK, nodeId);
    m.rev    = rev;
    m.status = status;
    // Bounded copies by hand: the memset above already terminated both, and
    // strncpy with a size-1 bound draws -Wstringop-truncation on some GCCs.
    for (size_t i = 0; field && field[i] && i < sizeof(m.field) - 1; i++)
        m.field[i] = field[i];
    for (size_t i = 0; reason && reason[i] && i < sizeof(m.reason) - 1; i++)
        m.reason[i] = reason[i];
}

/// Reassembly of a config sent in slices, for either direction.
///
/// SEQUENTIAL ON PURPOSE. §5 has the node ask for offset 0, then each next
/// offset, and send its own report the same way, so a slice that is not the
/// next expected one means something was lost or a new transfer began. The
/// assembler restarts on any offset-0 slice and ignores everything else that
/// does not follow on — it never tries to fill holes, because the sender is
/// going to start over from 0 anyway (a wake that runs out of time resumes
/// from 0 next wake) and hole-filling is where reassembly bugs live.
///
/// ~1 KB. The node keeps one; the collector keeps one per node that is
/// reporting, and can drop it when the report completes.
struct EnCfgAssembler {
    uint8_t  nodeId;
    uint16_t rev;
    uint16_t total;
    uint16_t have;       ///< bytes received in order so far
    char     doc[EN_CFG_MAX_TOTAL + 1];   ///< NUL-terminated once complete
};

enum EnCfgFeed : uint8_t {
    EN_CFG_FEED_IGNORED = 0,   ///< not the next slice of the current transfer
    EN_CFG_FEED_MORE    = 1,   ///< accepted; ask for / wait for offset `have`
    EN_CFG_FEED_DONE    = 2,   ///< accepted and complete: `doc` holds `total` bytes
};

static inline void espnowCfgReset(EnCfgAssembler& a) {
    a.nodeId = 0;
    a.rev    = 0;
    a.total  = 0;
    a.have   = 0;
    a.doc[0] = '\0';
}

/// Feed one validated CFG / CFG_REPORT frame. A `total == 0` frame carries no
/// document and is always IGNORED here — the caller handles it (it is the
/// "adopted rev" reply) before or instead of feeding.
static inline EnCfgFeed espnowCfgFeed(EnCfgAssembler& a, const CfgChunkMsg& m) {
    if (m.total == 0 || m.total > EN_CFG_MAX_TOTAL || m.len == 0) return EN_CFG_FEED_IGNORED;
    if ((uint32_t)m.offset + m.len > m.total) return EN_CFG_FEED_IGNORED;
    if (m.offset == 0) {
        a.nodeId = m.nodeId;
        a.rev    = m.rev;
        a.total  = m.total;
        a.have   = 0;
    } else if (m.nodeId != a.nodeId || m.rev != a.rev || m.total != a.total ||
               m.offset != a.have || a.total == 0) {
        return EN_CFG_FEED_IGNORED;
    }
    memcpy(a.doc + m.offset, m.data, m.len);
    a.have = (uint16_t)(m.offset + m.len);
    if (a.have < a.total) return EN_CFG_FEED_MORE;
    a.doc[a.total] = '\0';
    return EN_CFG_FEED_DONE;
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

// The configuration and DATA2 frames. The offsets are the ones espnowValidate()
// reads by index, so each is pinned where it is read.
static_assert(sizeof(CfgGetMsg)   == 8,   "CfgGetMsg: 4 + haveRev + offset");
static_assert(sizeof(CfgChunkMsg) == EN_CFG_CHUNK_HDR + EN_CFG_CHUNK_MAX,
              "CfgChunkMsg: 11-byte header + data");
static_assert(offsetof(CfgChunkMsg, data) == (size_t)EN_CFG_CHUNK_HDR,
              "espnowCfgChunkLen() assumes data follows an 11-byte header");
static_assert(offsetof(CfgChunkMsg, total)  == 6,  "espnowCfgChunkOk() reads total at [6]");
static_assert(offsetof(CfgChunkMsg, offset) == 8,  "espnowCfgChunkOk() reads offset at [8]");
static_assert(offsetof(CfgChunkMsg, len)    == 10, "espnowCfgChunkOk() reads len at [10]");
static_assert(offsetof(CfgGetMsg, offset)   == 6,  "espnowValidate() reads offset at [6]");
static_assert(sizeof(CfgAckMsg)   == 79,  "CfgAckMsg: 7 + field[24] + reason[48]");
static_assert(offsetof(CfgAckMsg, status) == 6, "espnowValidate() reads status at [6]");
static_assert(offsetof(CfgAckMsg, field)  == 7, "espnowValidate() reads field at [7]");
static_assert(sizeof(CfgChunkMsg) <= ESPNOW_MAX_FRAME, "a config slice must fit one frame");
static_assert(sizeof(CfgAckMsg)   <= ESPNOW_MAX_FRAME, "CFG_ACK must fit one frame");
static_assert(EN_CFG_MAX_TOTAL <= 0xFFFF, "total and offset are u16 on the wire");
static_assert(sizeof(Data2Header) == 12, "DATA2 shares DATA's 12-byte header");
static_assert(offsetof(Data2Header, count) == offsetof(DataMsg, count),
              "espnowData2Walk() reads count at [6], like DATA");
static_assert(sizeof(Data2Value)  == 6,  "a DATA2 value is id + index + float32");
static_assert(EN_DATA2_HDR + 3 + EN_DATA2_MAX_VALUES * 6 <= ESPNOW_MAX_FRAME,
              "one full DATA2 sample must fit a frame");
