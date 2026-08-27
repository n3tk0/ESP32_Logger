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
    CHECK_EQ((int)sizeof(WelcomeMsg),  51);

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
static void test_discover_signed_region() {
    DiscoverMsg d;
    CHECK_EQ((int)EN_DISCOVER_SIGNED_LEN, (int)offsetof(DiscoverMsg, tag));
    CHECK_EQ((int)(EN_DISCOVER_SIGNED_LEN + sizeof(d.tag)), (int)sizeof(DiscoverMsg));
}

int main() {
    RUN(test_sizes);
    RUN(test_round_trip);
    RUN(test_absent_sentinels);
    RUN(test_encode_decode_every_count);
    RUN(test_encode_rejects_bad_input);
    RUN(test_validate_rejects);
    RUN(test_validate_accepts_each_type);
    RUN(test_discover_signed_region);
    return SUMMARY();
}
