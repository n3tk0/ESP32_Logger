// Host unit tests for src/espnow/EspNowProto.h
//
// The header is compiled by both the collector and the battery node, so its
// layout IS the protocol. Two kinds of claim are checked here:
//
//   • that the packing helpers round-trip, and that every field's "not
//     measured" sentinel survives a trip through the wire format instead of
//     coming back as a plausible-looking zero;
//   • that espnowValidate() rejects every malformed frame it is handed —
//     tested with heap buffers sized to the frame, so a read past the end is
//     an ASan report and not a silent pass.
//
// The struct sizes are static_asserted in the header itself and re-checked at
// runtime below. That looks redundant and is not quite: the assertions fire on
// whichever target compiles first, and these run on the CI host, which is the
// only place all three of x86-64, Xtensa and RISC-V agreement can be observed.
#include <stdint.h>
#include <stdlib.h>

#include "src/espnow/EspNowProto.h"
#include "check.h"

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
static void test_sizes() {
    CHECK_EQ((int)sizeof(EnvSample),   12);
    CHECK_EQ((int)sizeof(DataMsg),     192);
    CHECK_EQ((int)sizeof(AckMsg),      14);
    CHECK_EQ((int)sizeof(DiscoverMsg), 22);
    CHECK_EQ((int)sizeof(WelcomeMsg),  65);

    // A one-sample frame is the common case and the one whose size decides the
    // node's airtime: 24 bytes against the 237 the JSON node posts.
    CHECK_EQ(espnowDataLen(1),  24);
    CHECK_EQ(espnowDataLen(15), 192);
    for (uint8_t c = 1; c <= ESPNOW_MAX_SAMPLES; c++)
        CHECK(espnowDataLen(c) <= ESPNOW_MAX_FRAME);
}

// ---------------------------------------------------------------------------
// Field packing
// ---------------------------------------------------------------------------
static void test_round_trip() {
    // Temperature: two decimals, both signs, and the rounding at the boundary.
    CHECK_EQ(enPackTemp(21.34f),  2134);
    CHECK_EQ(enPackTemp(-5.67f), -567);
    CHECK_EQ(enPackTemp(0.0f),    0);
    CHECK(enUnpackTemp(enPackTemp(21.34f)) > 21.339f);
    CHECK(enUnpackTemp(enPackTemp(21.34f)) < 21.341f);
    CHECK(enUnpackTemp(enPackTemp(-5.67f)) < -5.669f);

    CHECK_EQ(enPackRh(48.25f), 4825);
    CHECK_EQ(enPackRh(0.0f),   0);
    CHECK_EQ(enPackRh(100.0f), 10000);

    CHECK_EQ((int)enPackPress(101325.0f), 101325);
    CHECK_EQ((int)enPackMv(3874.0f),      3874);
}

// A zero is a reading and an absent value is not, and the difference has to
// survive the wire. This is the check that stops a BMP280 — no humidity
// sensor at all — from being reported as 0 %RH on the dashboard.
static void test_absent_sentinels() {
    CHECK(enIsAbsent(enUnpackTemp(EN_ABSENT_I16)));
    CHECK(enIsAbsent(enUnpackRh(EN_ABSENT_U16)));
    CHECK(enIsAbsent(enUnpackPress(EN_ABSENT_U32)));
    CHECK(enIsAbsent(enUnpackMv(EN_ABSENT_U16)));

    // NaN in, absent out.
    CHECK_EQ(enPackTemp(enNaN()),  EN_ABSENT_I16);
    CHECK_EQ(enPackRh(enNaN()),    EN_ABSENT_U16);
    CHECK_EQ((int64_t)enPackPress(enNaN()), (int64_t)EN_ABSENT_U32);
    CHECK_EQ(enPackMv(enNaN()),    EN_ABSENT_U16);

    // Out of range is absent too, not clamped: a clamped value reads as a
    // measurement, and a sensor reporting 300 %RH has not measured anything.
    CHECK_EQ(enPackRh(-0.1f),   EN_ABSENT_U16);
    CHECK_EQ(enPackRh(100.1f),  EN_ABSENT_U16);
    CHECK_EQ(enPackTemp(400.f), EN_ABSENT_I16);
    CHECK_EQ((int64_t)enPackPress(1000.0f),   (int64_t)EN_ABSENT_U32);
    CHECK_EQ((int64_t)enPackPress(500000.0f), (int64_t)EN_ABSENT_U32);

    // A real reading must never collide with a sentinel. -327.68 °C would, so
    // the range guard has to bite before the cast does.
    CHECK(enPackTemp(-327.68f) == EN_ABSENT_I16);
    CHECK(enPackTemp(-320.0f)  != EN_ABSENT_I16);

    // And a cleared sample is absent in every field, including the two that a
    // memset() to zero would have made look like real measurements.
    EnvSample s;
    enClearSample(s);
    CHECK(enIsAbsent(enUnpackTemp(s.t_c100)));
    CHECK(enIsAbsent(enUnpackRh(s.rh_x100)));
    CHECK(enIsAbsent(enUnpackPress(s.press_pa)));
    CHECK(enIsAbsent(enUnpackMv(s.vbat_mv)));
    CHECK_EQ(s.dt_s, 0);
}

// ---------------------------------------------------------------------------
// Encode / validate / decode
// ---------------------------------------------------------------------------
static DataMsg makeData(uint8_t count) {
    DataMsg m;
    memset(&m, 0, sizeof(m));
    m.magic  = ESPNOW_MAGIC;
    m.ver    = ESPNOW_PROTO_VER;
    m.type   = EN_MSG_DATA;
    m.nodeId = 7;
    m.seq    = 1234;
    m.count  = count;
    m.epoch  = 1750000000u;
    for (uint8_t i = 0; i < count; i++) {
        enClearSample(m.s[i]);
        m.s[i].dt_s    = (uint16_t)(i * 60);
        m.s[i].t_c100  = (int16_t)(2000 + i);
        m.s[i].vbat_mv = (uint16_t)(3900 - i);
    }
    return m;
}

static void test_encode_decode_every_count() {
    for (uint8_t count = 1; count <= ESPNOW_MAX_SAMPLES; count++) {
        const DataMsg src = makeData(count);
        const int n = espnowDataLen(count);

        // Exact-sized heap buffer: an encoder that wrote the full 192 bytes
        // regardless of count would be a heap overflow here, not a slow frame
        // nobody noticed.
        uint8_t* buf = (uint8_t*)malloc((size_t)n);
        CHECK_EQ(espnowEncodeData(src, buf, (size_t)n), n);

        uint8_t type = 0;
        CHECK(espnowValidate(buf, n, type));
        CHECK_EQ(type, (uint8_t)EN_MSG_DATA);

        DataMsg got;
        espnowDecodeData(buf, n, got);
        CHECK_EQ(got.count, count);
        CHECK_EQ(got.seq,   1234);
        CHECK_EQ(got.nodeId, 7);
        CHECK_EQ((long long)got.epoch, 1750000000LL);
        for (uint8_t i = 0; i < count; i++) {
            CHECK_EQ(got.s[i].dt_s,    (uint16_t)(i * 60));
            CHECK_EQ(got.s[i].t_c100,  (int16_t)(2000 + i));
            CHECK_EQ(got.s[i].vbat_mv, (uint16_t)(3900 - i));
            CHECK(enIsAbsent(enUnpackRh(got.s[i].rh_x100)));
        }
        // Samples past the count are zeroed by the decoder, so a caller that
        // ignores `count` gets zeros rather than the previous frame's data.
        for (uint8_t i = count; i < ESPNOW_MAX_SAMPLES; i++)
            CHECK_EQ(got.s[i].t_c100, 0);

        free(buf);
    }
}

static void test_encode_rejects_bad_input() {
    const DataMsg m = makeData(3);
    uint8_t buf[ESPNOW_MAX_FRAME];

    CHECK_EQ(espnowEncodeData(m, buf, (size_t)espnowDataLen(3) - 1), -1);  // no room
    CHECK_EQ(espnowEncodeData(m, nullptr, sizeof(buf)), -1);

    DataMsg zero = m; zero.count = 0;
    CHECK_EQ(espnowEncodeData(zero, buf, sizeof(buf)), -1);
    DataMsg over = m; over.count = ESPNOW_MAX_SAMPLES + 1;
    CHECK_EQ(espnowEncodeData(over, buf, sizeof(buf)), -1);
}

// The receive path's whole defence. Each case gets a buffer sized exactly to
// the length being claimed, so a validator that reads a field before checking
// the length is caught by the sanitizer rather than by luck.
static void test_validate_rejects() {
    uint8_t type = 0;

    CHECK(!espnowValidate(nullptr, 24, type));
    CHECK(!espnowValidate((const uint8_t*)"\xE5\x01", 2, type));

    // Truncated to under a header: the count byte at [6] must not be read.
    {
        const DataMsg m = makeData(1);
        for (int len = 0; len < espnowDataLen(1); len++) {
            uint8_t* b = (uint8_t*)malloc(len ? (size_t)len : 1);
            memcpy(b, &m, (size_t)len);
            CHECK(!espnowValidate(b, len, type));
            free(b);
        }
    }

    // Wrong magic, wrong version, unknown type.
    {
        const int n = espnowDataLen(2);
        DataMsg m = makeData(2);
        uint8_t* b = (uint8_t*)malloc((size_t)n);

        memcpy(b, &m, (size_t)n); b[0] = 0xE4;
        CHECK(!espnowValidate(b, n, type));

        memcpy(b, &m, (size_t)n); b[1] = ESPNOW_PROTO_VER + 1;
        CHECK(!espnowValidate(b, n, type));

        memcpy(b, &m, (size_t)n); b[2] = 99;
        CHECK(!espnowValidate(b, n, type));

        // count out of range, at both ends
        memcpy(b, &m, (size_t)n); b[6] = 0;
        CHECK(!espnowValidate(b, n, type));
        memcpy(b, &m, (size_t)n); b[6] = ESPNOW_MAX_SAMPLES + 1;
        CHECK(!espnowValidate(b, n, type));

        free(b);
    }

    // Length that disagrees with the declared count, one byte either way. This
    // is the case that matters: a burst whose tail was lost must be dropped,
    // not parsed into whatever the radio driver left in the buffer.
    for (uint8_t count = 1; count <= ESPNOW_MAX_SAMPLES; count++) {
        const DataMsg m = makeData(count);
        const int n = espnowDataLen(count);
        for (int delta = -1; delta <= 1; delta += 2) {
            const int len = n + delta;
            if (len < 0) continue;
            uint8_t* b = (uint8_t*)malloc((size_t)len);
            memcpy(b, &m, (size_t)(len < n ? len : n));
            if (len > n) b[len - 1] = 0;
            CHECK(!espnowValidate(b, len, type));
            free(b);
        }
    }
}

static void test_validate_accepts_each_type() {
    uint8_t type = 0;

    AckMsg a; memset(&a, 0, sizeof(a));
    a.magic = ESPNOW_MAGIC; a.ver = ESPNOW_PROTO_VER; a.type = EN_MSG_ACK;
    CHECK(espnowValidate((const uint8_t*)&a, sizeof(a), type));
    CHECK_EQ(type, (uint8_t)EN_MSG_ACK);
    CHECK(!espnowValidate((const uint8_t*)&a, sizeof(a) - 1, type));

    DiscoverMsg d; memset(&d, 0, sizeof(d));
    d.magic = ESPNOW_MAGIC; d.ver = ESPNOW_PROTO_VER; d.type = EN_MSG_DISCOVER;
    CHECK(espnowValidate((const uint8_t*)&d, sizeof(d), type));
    CHECK_EQ(type, (uint8_t)EN_MSG_DISCOVER);

    WelcomeMsg w; memset(&w, 0, sizeof(w));
    w.magic = ESPNOW_MAGIC; w.ver = ESPNOW_PROTO_VER; w.type = EN_MSG_WELCOME;
    CHECK(espnowValidate((const uint8_t*)&w, sizeof(w), type));
    CHECK_EQ(type, (uint8_t)EN_MSG_WELCOME);

    // The SSID field has room for 32 characters and a terminator, which is the
    // maximum an 802.11 SSID can be — a truncating copy here would leave a
    // node unable to find its own network again after a channel move.
    CHECK_EQ((int)sizeof(w.ssid), 33);
}

// The tag must cover every byte before it and no byte after it. Getting this
// wrong is not a build error and not a runtime error — it is a signature that
// verifies while leaving part of the frame unauthenticated.
static void test_signed_regions() {
    DiscoverMsg d;
    CHECK_EQ((int)EN_DISCOVER_SIGNED_LEN, (int)offsetof(DiscoverMsg, tag));
    CHECK_EQ((int)(EN_DISCOVER_SIGNED_LEN + sizeof(d.tag)), (int)sizeof(DiscoverMsg));

    // WELCOME is signed for a reason worth restating: ESP-NOW decrypts only
    // from a peer already added with the key, so the node cannot receive an
    // encrypted reply from a collector whose MAC the reply is what tells it.
    // The frame therefore goes out in the clear and carries a tag instead.
    WelcomeMsg w;
    CHECK_EQ((int)EN_WELCOME_SIGNED_LEN, (int)offsetof(WelcomeMsg, tag));
    CHECK_EQ((int)(EN_WELCOME_SIGNED_LEN + sizeof(w.tag)), (int)sizeof(WelcomeMsg));

    // And the target it is addressed to must be inside the signed region: a
    // tag that did not cover it would let anyone in range retarget a valid
    // WELCOME at a different node.
    CHECK((int)offsetof(WelcomeMsg, target) < (int)EN_WELCOME_SIGNED_LEN);
    CHECK((int)(offsetof(WelcomeMsg, target) + 6) <= (int)EN_WELCOME_SIGNED_LEN);
}

// ===========================================================================
// Remote configuration and DATA2 (docs/NODE_CONFIG.md §5)
// ===========================================================================

// Copy `len` bytes into a heap buffer of exactly that size, so a validator
// that reads one byte too far is an ASan report rather than a silent pass.
static uint8_t* exact(const void* src, int len) {
    uint8_t* b = (uint8_t*)malloc(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(b, src, (size_t)len);
    return b;
}

static bool validates(const void* src, int len, uint8_t expectType) {
    uint8_t* b = exact(src, len);
    uint8_t type = 0;
    const bool ok = espnowValidate(b, len, type);
    free(b);
    return ok && type == expectType;
}

static void test_cfg_layout_and_numbers() {
    // The numbers are the contract's; a renumbering is a protocol break that
    // compiles, so it is pinned here as well as in the header.
    CHECK_EQ((int)EN_MSG_CFG_GET, 5);
    CHECK_EQ((int)EN_MSG_CFG, 6);
    CHECK_EQ((int)EN_MSG_CFG_ACK, 7);
    CHECK_EQ((int)EN_MSG_CFG_REPORT, 8);
    CHECK_EQ((int)EN_MSG_DATA2, 9);
    CHECK_EQ((int)EN_ACK_CFG_PENDING, 2);
    // A new flag, not a new version: an old node must keep talking.
    CHECK_EQ((int)ESPNOW_PROTO_VER, 1);
    CHECK((EN_ACK_CFG_PENDING & EN_ACK_REDISCOVER) == 0);

    CHECK_EQ((int)sizeof(CfgGetMsg), 8);
    CHECK_EQ((int)sizeof(CfgChunkMsg), 211);
    CHECK_EQ((int)sizeof(CfgAckMsg), 79);
    CHECK_EQ((int)sizeof(Data2Header), 12);
    CHECK_EQ((int)sizeof(Data2Value), 6);
    CHECK_EQ(espnowCfgChunkLen(0), 11);
    CHECK_EQ(espnowCfgChunkLen(EN_CFG_CHUNK_MAX), 211);
    CHECK(espnowCfgChunkLen(EN_CFG_CHUNK_MAX) <= ESPNOW_MAX_FRAME);
    CHECK_EQ(espnowData2SampleLen(0), 3);
    CHECK_EQ(espnowData2SampleLen(EN_DATA2_MAX_VALUES), 57);
}

// ---------------------------------------------------------------------------
static void test_data2_round_trip() {
    uint8_t buf[ESPNOW_MAX_FRAME];
    int len = espnowData2Begin(buf, sizeof(buf), 9, 4321, EN_FLAG_FIRST_BOOT, 1750000000u);
    CHECK_EQ(len, 12);

    // Three samples of different shapes: a full one, an empty one (a node
    // whose sensors all failed still has to send something to get its ACK),
    // and one with a probe index.
    Data2Value a[3] = { {1, 0, 21.5f}, {2, 0, 48.0f}, {14, 0, 3.91f} };
    Data2Value c[2] = { {13, 0, 18.25f}, {13, 2, -4.5f} };
    CHECK(espnowData2Append(buf, sizeof(buf), len, 120, a, 3));
    CHECK(espnowData2Append(buf, sizeof(buf), len, 60, nullptr, 0));
    CHECK(espnowData2Append(buf, sizeof(buf), len, 0, c, 2));
    CHECK_EQ(len, 12 + 21 + 3 + 15);

    CHECK(validates(buf, len, EN_MSG_DATA2));

    Data2Header h;
    Data2Cursor cur;
    espnowData2Open(buf, len, h, cur);
    CHECK_EQ(h.magic, ESPNOW_MAGIC);
    CHECK_EQ(h.type, (uint8_t)EN_MSG_DATA2);
    CHECK_EQ(h.nodeId, 9);
    CHECK_EQ(h.seq, 4321);
    CHECK_EQ(h.count, 3);
    CHECK_EQ(h.flags, (uint8_t)EN_FLAG_FIRST_BOOT);
    CHECK_EQ((long long)h.epoch, 1750000000LL);

    Data2Sample s;
    CHECK(espnowData2Next(cur, s));
    CHECK_EQ(s.dt_s, 120);
    CHECK_EQ(s.n, 3);
    CHECK_EQ(s.v[0].metric, 1);
    CHECK(s.v[0].value == 21.5f);
    CHECK_EQ(s.v[2].metric, 14);
    CHECK(s.v[2].value == 3.91f);

    CHECK(espnowData2Next(cur, s));
    CHECK_EQ(s.dt_s, 60);
    CHECK_EQ(s.n, 0);

    CHECK(espnowData2Next(cur, s));
    CHECK_EQ(s.n, 2);
    CHECK_EQ(s.v[1].metric, 13);
    CHECK_EQ(s.v[1].index, 2);
    CHECK(s.v[1].value == -4.5f);

    CHECK(!espnowData2Next(cur, s));   // exactly three, then done
}

// ---------------------------------------------------------------------------
static void test_data2_packs_whole_samples_only() {
    // Full samples (9 values, 57 bytes) until the frame is full: (250-12)/57
    // is 4. The fifth must be refused WITHOUT touching the frame, so the node
    // can send what it has and carry the fifth to the next frame.
    uint8_t buf[ESPNOW_MAX_FRAME + 64];
    int len = espnowData2Begin(buf, sizeof(buf), 1, 1, 0, 0);
    Data2Value v[EN_DATA2_MAX_VALUES];
    for (uint8_t i = 0; i < EN_DATA2_MAX_VALUES; i++) v[i] = { (uint8_t)(i + 1), 0, (float)i };

    int fitted = 0;
    while (espnowData2Append(buf, sizeof(buf), len, (uint16_t)(fitted * 60), v,
                             EN_DATA2_MAX_VALUES))
        fitted++;
    CHECK_EQ(fitted, 4);
    CHECK_EQ(len, 12 + 4 * 57);
    CHECK_EQ(buf[6], 4);
    CHECK(len <= ESPNOW_MAX_FRAME);   // even though the buffer was bigger
    CHECK(validates(buf, len, EN_MSG_DATA2));

    // A small sample still fits in what is left (250 - 240 = 10 bytes).
    CHECK(espnowData2Append(buf, sizeof(buf), len, 999, v, 1));
    CHECK_EQ(buf[6], 5);
    CHECK(validates(buf, len, EN_MSG_DATA2));

    // Bad input.
    int l2 = espnowData2Begin(buf, sizeof(buf), 1, 1, 0, 0);
    CHECK(!espnowData2Append(buf, sizeof(buf), l2, 0, v, EN_DATA2_MAX_VALUES + 1));
    CHECK(!espnowData2Append(buf, sizeof(buf), l2, 0, nullptr, 2));
    CHECK(!espnowData2Append(buf, 20, l2, 0, v, 2));   // cap smaller than the sample
    CHECK_EQ(l2, 12);
    CHECK_EQ(espnowData2Begin(buf, 11, 1, 1, 0, 0), -1);
    // A begun frame with no sample is not a frame.
    CHECK(!validates(buf, 12, EN_MSG_DATA2));
}

// ---------------------------------------------------------------------------
static void test_data2_validate_rejects() {
    uint8_t buf[ESPNOW_MAX_FRAME];
    int len = espnowData2Begin(buf, sizeof(buf), 3, 7, 0, 0);
    Data2Value v[2] = { {1, 0, 20.0f}, {2, 0, 50.0f} };
    espnowData2Append(buf, sizeof(buf), len, 0, v, 2);
    espnowData2Append(buf, sizeof(buf), len, 60, v, 1);
    CHECK(validates(buf, len, EN_MSG_DATA2));

    // Every truncation, and one byte of trailing junk: each sample's length is
    // implied by its own `n`, so any mismatch with `len` is a lost tail or a
    // frame that is not what it claims.
    for (int l = 0; l < len; l++) CHECK(!validates(buf, l, EN_MSG_DATA2));
    {
        uint8_t more[ESPNOW_MAX_FRAME];
        memcpy(more, buf, (size_t)len);
        more[len] = 0;
        CHECK(!validates(more, len + 1, EN_MSG_DATA2));
    }

    uint8_t b[ESPNOW_MAX_FRAME];
    // count says more samples than there are
    memcpy(b, buf, (size_t)len); b[6] = 3;
    CHECK(!validates(b, len, EN_MSG_DATA2));
    // count says fewer — the rest is junk
    memcpy(b, buf, (size_t)len); b[6] = 1;
    CHECK(!validates(b, len, EN_MSG_DATA2));
    memcpy(b, buf, (size_t)len); b[6] = 0;
    CHECK(!validates(b, len, EN_MSG_DATA2));
    // a sample claiming more values than a sample may hold
    memcpy(b, buf, (size_t)len); b[12 + 2] = EN_DATA2_MAX_VALUES + 1;
    CHECK(!validates(b, len, EN_MSG_DATA2));
    // a sample claiming one value more than it carries
    memcpy(b, buf, (size_t)len); b[12 + 2] = 3;
    CHECK(!validates(b, len, EN_MSG_DATA2));

    // Longer than the radio can carry, even if internally consistent.
    {
        uint8_t big[ESPNOW_MAX_FRAME + 16];
        int bl = espnowData2Begin(big, sizeof(big), 1, 1, 0, 0);
        // Hand-built, because espnowData2Append() will not go past 250: empty
        // samples (3 bytes each) until the frame is just over the limit.
        big[6] = 0;
        while (bl + 3 <= (int)sizeof(big) - 1) {
            big[bl] = 0; big[bl + 1] = 0; big[bl + 2] = 0;
            bl += 3; big[6]++;
            if (bl > ESPNOW_MAX_FRAME) break;
        }
        CHECK(bl > ESPNOW_MAX_FRAME);
        CHECK(!validates(big, bl, EN_MSG_DATA2));
    }
}

// ---------------------------------------------------------------------------
static void test_cfg_chunks_slice_a_document() {
    // A 450-byte document goes as 200 + 200 + 50, each slice valid on its own
    // and saying where it sits.
    char doc[451];
    for (int i = 0; i < 450; i++) doc[i] = (char)('a' + i % 26);
    doc[450] = '\0';

    CfgChunkMsg m;
    int n = espnowFillCfgChunk(m, EN_MSG_CFG, 4, 17, doc, 450, 0);
    CHECK_EQ(n, 211);
    CHECK_EQ(m.len, 200);
    CHECK(validates(&m, n, EN_MSG_CFG));
    n = espnowFillCfgChunk(m, EN_MSG_CFG, 4, 17, doc, 450, 200);
    CHECK_EQ(m.len, 200);
    CHECK(validates(&m, n, EN_MSG_CFG));
    n = espnowFillCfgChunk(m, EN_MSG_CFG_REPORT, 4, 17, doc, 450, 400);
    CHECK_EQ(n, 61);
    CHECK_EQ(m.len, 50);
    CHECK(validates(&m, n, EN_MSG_CFG_REPORT));
    CHECK(memcmp(m.data, doc + 400, 50) == 0);

    // Outside the document, oversized, or the wrong type: refused.
    CHECK_EQ(espnowFillCfgChunk(m, EN_MSG_CFG, 4, 17, doc, 450, 450), -1);
    CHECK_EQ(espnowFillCfgChunk(m, EN_MSG_CFG, 4, 17, doc, EN_CFG_MAX_TOTAL + 1, 0), -1);
    CHECK_EQ(espnowFillCfgChunk(m, EN_MSG_DATA, 4, 17, doc, 450, 0), -1);
    CHECK_EQ(espnowFillCfgChunk(m, EN_MSG_CFG, 4, 17, nullptr, 450, 0), -1);

    // The data-less "your local config is now rev N" reply.
    n = espnowFillCfgChunk(m, EN_MSG_CFG, 4, 18, nullptr, 0, 0);
    CHECK_EQ(n, 11);
    CHECK_EQ(m.total, 0);
    CHECK_EQ(m.rev, 18);
    CHECK(validates(&m, n, EN_MSG_CFG));
}

// ---------------------------------------------------------------------------
static void test_cfg_chunk_validate_rejects() {
    char doc[300];
    memset(doc, 'x', sizeof(doc));
    CfgChunkMsg m;
    const int n = espnowFillCfgChunk(m, EN_MSG_CFG, 1, 2, doc, 300, 200);
    CHECK_EQ(n, 111);
    CHECK(validates(&m, n, EN_MSG_CFG));

    for (int l = 0; l < n; l++) CHECK(!validates(&m, l, EN_MSG_CFG));
    CHECK(!validates(&m, n + 1, EN_MSG_CFG));

    CfgChunkMsg b;
    b = m; b.len = 101;                         // len disagrees with the frame
    CHECK(!validates(&b, n, EN_MSG_CFG));
    b = m; b.offset = 250;                      // slice runs past total
    CHECK(!validates(&b, n, EN_MSG_CFG));
    b = m; b.total = EN_CFG_MAX_TOTAL + 1;      // over the ceiling
    CHECK(!validates(&b, n, EN_MSG_CFG));
    b = m; b.total = 0;                         // "adopted" reply with data
    CHECK(!validates(&b, n, EN_MSG_CFG));
    // An empty slice in the middle of a document would never advance.
    b = m; b.len = 0;
    CHECK(!validates(&b, espnowCfgChunkLen(0), EN_MSG_CFG));
    // 201 bytes of data does not fit the struct and is refused by len alone.
    b = m; b.len = EN_CFG_CHUNK_MAX + 1;
    CHECK(!validates(&b, (int)sizeof(CfgChunkMsg), EN_MSG_CFG));
}

// ---------------------------------------------------------------------------
static void test_cfg_assembler() {
    char doc[431];
    for (int i = 0; i < 430; i++) doc[i] = (char)('A' + i % 26);
    doc[430] = '\0';

    EnCfgAssembler a;
    espnowCfgReset(a);
    CfgChunkMsg m;

    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 9, doc, 430, 0);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_MORE);
    CHECK_EQ(a.have, 200);

    // The same slice again (a radio retry) is not the next one: ignored,
    // nothing moves.
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_MORE);   // offset 0 restarts...
    CHECK_EQ(a.have, 200);                                        // ...to the same place

    // A slice from further on is not the next one either.
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 9, doc, 430, 400);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_IGNORED);
    CHECK_EQ(a.have, 200);

    // Nor is the right offset of a different rev or node.
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 10, doc, 430, 200);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_IGNORED);
    espnowFillCfgChunk(m, EN_MSG_CFG, 6, 9, doc, 430, 200);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_IGNORED);

    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 9, doc, 430, 200);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_MORE);
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 9, doc, 430, 400);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_DONE);
    CHECK_EQ(a.have, 430);
    CHECK_EQ(a.rev, 9);
    CHECK_EQ((int)strlen(a.doc), 430);
    CHECK(strcmp(a.doc, doc) == 0);

    // A new offset 0 starts over, whatever came before.
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 11, doc, 150, 0);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_DONE);
    CHECK_EQ(a.rev, 11);
    CHECK_EQ((int)strlen(a.doc), 150);

    // The data-less reply carries no document.
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 12, nullptr, 0, 0);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_IGNORED);

    // A fresh assembler does not accept a middle slice as a start.
    espnowCfgReset(a);
    espnowFillCfgChunk(m, EN_MSG_CFG, 5, 9, doc, 430, 200);
    CHECK_EQ((int)espnowCfgFeed(a, m), (int)EN_CFG_FEED_IGNORED);
}

// ---------------------------------------------------------------------------
// The collector has ONE report assembler for every node. Nodes that boot
// together (a power cut) report together, and another node's first slice
// must not wipe a transfer that is still moving; a stalled one is given up.
static void test_cfg_assembler_held_against_another_node() {
    char doc[431];
    for (int i = 0; i < 430; i++) doc[i] = (char)('a' + i % 26);
    doc[430] = '\0';

    EnCfgAssembler a;
    espnowCfgReset(a);
    CfgChunkMsg mA, mB;

    // Empty: nothing to hold, anybody may start.
    espnowFillCfgChunk(mB, EN_MSG_CFG_REPORT, 6, 3, doc, 300, 0);
    CHECK(!espnowCfgHeld(a, mB, 0));

    // Node 5 is one slice into its report.
    espnowFillCfgChunk(mA, EN_MSG_CFG_REPORT, 5, 9, doc, 430, 0);
    CHECK(!espnowCfgHeld(a, mA, 0));
    CHECK_EQ((int)espnowCfgFeed(a, mA), (int)EN_CFG_FEED_MORE);

    // Node 6's first slice, a few ms later: held off ...
    CHECK(espnowCfgHeld(a, mB, 3));
    CHECK(espnowCfgHeld(a, mB, EN_CFG_HOLD_MS - 1));
    // ... node 6's later slices are not the next one anyway ...
    espnowFillCfgChunk(mB, EN_MSG_CFG_REPORT, 6, 3, doc, 300, 200);
    CHECK(!espnowCfgHeld(a, mB, 3));
    CHECK_EQ((int)espnowCfgFeed(a, mB), (int)EN_CFG_FEED_IGNORED);
    // ... and node 5's own slice 0 (its retry) always restarts.
    CHECK(!espnowCfgHeld(a, mA, 3));

    // A transfer idle for the whole per-wake budget is given up.
    espnowFillCfgChunk(mB, EN_MSG_CFG_REPORT, 6, 3, doc, 300, 0);
    CHECK(!espnowCfgHeld(a, mB, EN_CFG_HOLD_MS));
    CHECK(!espnowCfgHeld(a, mB, 60000));

    // Played out: node 5's slices and node 6's interleaved, ms apart. Node 5
    // lands whole; node 6 is dropped (and reports again on a later wake).
    espnowCfgReset(a);
    int done5 = 0;
    for (uint16_t off = 0; off < 430; off = (uint16_t)(off + 200)) {
        espnowFillCfgChunk(mA, EN_MSG_CFG_REPORT, 5, 9, doc, 430, off);
        if (!espnowCfgHeld(a, mA, 2) && espnowCfgFeed(a, mA) == EN_CFG_FEED_DONE) done5++;
        espnowFillCfgChunk(mB, EN_MSG_CFG_REPORT, 6, 3, doc, 300, off < 300 ? off : 200);
        if (!espnowCfgHeld(a, mB, 2)) espnowCfgFeed(a, mB);
    }
    CHECK_EQ(done5, 1);
    CHECK_EQ(a.nodeId, 5);
    CHECK_EQ(a.have, 430);
    CHECK(memcmp(a.doc, doc, 430) == 0);
    // Complete: not "in progress", so it holds nobody off (the collector's
    // own ready flag keeps it until the tick has stored it).
    espnowFillCfgChunk(mB, EN_MSG_CFG_REPORT, 6, 3, doc, 300, 0);
    CHECK(!espnowCfgHeld(a, mB, 0));
}

// ---------------------------------------------------------------------------
static void test_cfg_get_and_ack() {
    CfgGetMsg g;
    espnowFillCfgGet(g, 3, 4, 200);
    CHECK(validates(&g, sizeof(g), EN_MSG_CFG_GET));
    CHECK(!validates(&g, sizeof(g) - 1, EN_MSG_CFG_GET));
    CHECK_EQ(g.haveRev, 4);
    CHECK_EQ(g.offset, 200);
    // An offset no document can have.
    espnowFillCfgGet(g, 3, 4, EN_CFG_MAX_TOTAL);
    CHECK(!validates(&g, sizeof(g), EN_MSG_CFG_GET));

    CfgAckMsg a;
    espnowFillCfgAck(a, 3, 5, EN_CFG_REJECTED, "sensors[1].pin", "GPIO6 is the SPI flash bus");
    CHECK(validates(&a, sizeof(a), EN_MSG_CFG_ACK));
    CHECK(!validates(&a, sizeof(a) - 1, EN_MSG_CFG_ACK));
    CHECK_STREQ(a.field, "sensors[1].pin");
    CHECK_STREQ(a.reason, "GPIO6 is the SPI flash bus");
    CHECK_EQ(a.rev, 5);

    // Over-long strings are cut and still terminated.
    espnowFillCfgAck(a, 3, 5, EN_CFG_REJECTED,
                     "a-field-path-that-is-far-too-long-for-the-frame",
                     "a reason that goes on and on well past the forty-eight bytes it has");
    CHECK_EQ((int)strlen(a.field), (int)EN_CFG_FIELD_LEN - 1);
    CHECK_EQ((int)strlen(a.reason), (int)EN_CFG_REASON_LEN - 1);
    CHECK(validates(&a, sizeof(a), EN_MSG_CFG_ACK));

    espnowFillCfgAck(a, 3, 6, EN_CFG_OK, nullptr, nullptr);
    CHECK(validates(&a, sizeof(a), EN_MSG_CFG_ACK));
    CHECK_STREQ(a.field, "");

    // An unknown status, and strings with no terminator, are refused: the
    // collector reads both as C strings.
    CfgAckMsg b = a; b.status = 2;
    CHECK(!validates(&b, sizeof(b), EN_MSG_CFG_ACK));
    b = a; memset(b.field, 'f', sizeof(b.field));
    CHECK(!validates(&b, sizeof(b), EN_MSG_CFG_ACK));
    b = a; memset(b.reason, 'r', sizeof(b.reason));
    CHECK(!validates(&b, sizeof(b), EN_MSG_CFG_ACK));
}

int main() {
    RUN(test_sizes);
    RUN(test_round_trip);
    RUN(test_absent_sentinels);
    RUN(test_encode_decode_every_count);
    RUN(test_encode_rejects_bad_input);
    RUN(test_validate_rejects);
    RUN(test_validate_accepts_each_type);
    RUN(test_signed_regions);
    RUN(test_cfg_layout_and_numbers);
    RUN(test_data2_round_trip);
    RUN(test_data2_packs_whole_samples_only);
    RUN(test_data2_validate_rejects);
    RUN(test_cfg_chunks_slice_a_document);
    RUN(test_cfg_chunk_validate_rejects);
    RUN(test_cfg_assembler);
    RUN(test_cfg_assembler_held_against_another_node);
    RUN(test_cfg_get_and_ack);
    return SUMMARY();
}
