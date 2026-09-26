// Host unit tests for node firmware updates — docs/NODE_OTA.md.
//
//   • src/nodecfg/FwImage.h: the marker scanner (whole, split at every byte,
//     two markers, junk around it, a prefix that is not one), the image
//     header checks, the C3 image id;
//   • src/espnow/EspNowProto.h: the FW_GET / FW / FW_DONE frames — layouts,
//     build and validate, and every malformed shape the validator must drop.
//
// Frames are validated from heap buffers sized to the frame, so a read past
// the end is an ASan report rather than a silent pass.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "src/espnow/EspNowProto.h"
#include "src/nodecfg/FwImage.h"
#include "check.h"

using namespace nodefw;

// ---------------------------------------------------------------------------
// Marker
// ---------------------------------------------------------------------------

static std::vector<uint8_t> image(const std::string& before, const char* marker,
                                  const std::string& after) {
    std::vector<uint8_t> v(before.begin(), before.end());
    if (marker) {
        const size_t n = strlen(marker);
        v.insert(v.end(), marker, marker + n);
        v.push_back(0);
    }
    v.insert(v.end(), after.begin(), after.end());
    return v;
}

static Kind scanAll(const std::vector<uint8_t>& v, MarkerScan& s, size_t step) {
    scanBegin(s);
    for (size_t i = 0; i < v.size(); i += step) {
        const size_t n = (v.size() - i < step) ? v.size() - i : step;
        scanFeed(s, v.data() + i, n);
    }
    return scanKind(s);
}

static void test_marker_text() {
    CHECK_STREQ(NODEFW_MARKER_TEXT("esp8266", "2026.10.1"), "NODEFW1|esp8266|2026.10.1|");
    // The scanner's own prefix, decoded, is the literal's.
    char p[9] = {0};
    for (uint8_t i = 0; i < detail::PREFIX_LEN; i++) p[i] = (char)detail::prefixAt(i);
    CHECK_STREQ(p, "NODEFW1|");
}

static void test_marker_found() {
    const auto v = image(std::string(3000, '\x55'), NODEFW_MARKER_TEXT("espnow-c3", "2026.10.1"),
                         std::string(500, '\xAA'));
    for (size_t step : {1u, 2u, 3u, 7u, 64u, 4096u}) {
        MarkerScan s;
        CHECK_EQ(scanAll(v, s, step), KIND_ESPNOW_C3);
        CHECK_STREQ(s.ver, "2026.10.1");
    }
}

static void test_marker_split_everywhere() {
    const auto v = image("xxNODEFxx", NODEFW_MARKER_TEXT("esp8266", "1.2.3-rc_4+b"), "yy");
    for (size_t cut = 0; cut <= v.size(); cut++) {
        MarkerScan s;
        scanBegin(s);
        scanFeed(s, v.data(), cut);
        scanFeed(s, v.data() + cut, v.size() - cut);
        CHECK_EQ(scanKind(s), KIND_ESP8266);
        CHECK_STREQ(s.ver, "1.2.3-rc_4+b");
    }
}

static void test_marker_absent_or_bad() {
    MarkerScan s;
    CHECK_EQ(scanAll(image("no marker at all", nullptr, ""), s, 5), KIND_NONE);
    // Unknown kind, empty version, bad characters, missing trailing bar, too long.
    CHECK_EQ(scanAll(image("", "NODEFW1|esp32|1.0|", ""), s, 3), KIND_NONE);
    CHECK_EQ(scanAll(image("", "NODEFW1|esp8266||", ""), s, 3), KIND_NONE);
    CHECK_EQ(scanAll(image("", "NODEFW1|esp8266|1 0|", ""), s, 3), KIND_NONE);
    CHECK_EQ(scanAll(image("", "NODEFW1|esp8266|1.0", ""), s, 3), KIND_NONE);
    CHECK_EQ(scanAll(image("", "NODEFW1|esp8266|1.0|x", ""), s, 3), KIND_NONE);
    CHECK_EQ(scanAll(image("", ("NODEFW1|esp8266|" + std::string(60, '1') + "|").c_str(), ""), s, 3),
             KIND_NONE);
    // A near-prefix right before the real one still finds it.
    CHECK_EQ(scanAll(image("NODEFW", NODEFW_MARKER_TEXT("esp8266", "1"), ""), s, 1), KIND_ESP8266);
    CHECK_EQ(scanAll(image("NNODEFW1", NODEFW_MARKER_TEXT("esp8266", "1"), ""), s, 1), KIND_ESP8266);
}

static void test_marker_two() {
    MarkerScan s;
    std::vector<uint8_t> v = image("", NODEFW_MARKER_TEXT("esp8266", "1"), "zz");
    auto w = image("", NODEFW_MARKER_TEXT("esp8266", "2"), "");
    v.insert(v.end(), w.begin(), w.end());
    CHECK_EQ(scanAll(v, s, 4), KIND_ESP8266);
    CHECK_STREQ(s.ver, "1");                       // the first one names the version
    w = image("", NODEFW_MARKER_TEXT("espnow-c3", "1"), "");
    v.insert(v.end(), w.begin(), w.end());
    CHECK_EQ(scanAll(v, s, 4), KIND_NONE);         // two kinds: nobody's image
}

static void test_kind_names() {
    CHECK_EQ(kindFromName("esp8266"), KIND_ESP8266);
    CHECK_EQ(kindFromName("espnow-c3"), KIND_ESPNOW_C3);
    CHECK_EQ(kindFromName("ESP8266"), KIND_NONE);
    CHECK_EQ(kindFromName(nullptr), KIND_NONE);
    CHECK_STREQ(kindName(KIND_ESPNOW_C3), "espnow-c3");
    CHECK_STREQ(kindName((Kind)9), "");
    CHECK_EQ(maxSize(KIND_ESPNOW_C3), EN_FW_MAX_SIZE);
    CHECK(maxSize(KIND_ESP8266) < maxSize(KIND_ESPNOW_C3));
    CHECK_EQ(maxSize(KIND_NONE), 0u);
}

// ---------------------------------------------------------------------------
// Image header
// ---------------------------------------------------------------------------

static std::vector<uint8_t> c3Head() {
    std::vector<uint8_t> h(HEAD_NEED, 0);
    h[0]  = 0xE9;
    h[12] = 0x05;
    h[13] = 0x00;
    const uint32_t m = APP_DESC_MAGIC;
    memcpy(&h[APP_DESC_OFFSET], &m, 4);
    const uint8_t sha[4] = { 0x78, 0x56, 0x34, 0x12 };
    memcpy(&h[ELF_SHA_OFFSET], sha, 4);
    return h;
}

static void test_head() {
    auto h = c3Head();
    CHECK(headOk(KIND_ESPNOW_C3, h.data(), h.size()));
    CHECK_EQ(c3ImageId(h.data()), 0x12345678u);
    CHECK(!headOk(KIND_ESPNOW_C3, h.data(), h.size() - 1));   // too short to judge
    CHECK(headOk(KIND_ESP8266, h.data(), 1));
    CHECK(!headOk(KIND_NONE, h.data(), h.size()));
    h[12] = 0x09;                                             // an ESP32-S3
    CHECK(!headOk(KIND_ESPNOW_C3, h.data(), h.size()));
    h = c3Head();
    h[APP_DESC_OFFSET] ^= 1;
    CHECK(!headOk(KIND_ESPNOW_C3, h.data(), h.size()));
    h = c3Head();
    h[0] = 0xEA;
    CHECK(!headOk(KIND_ESPNOW_C3, h.data(), h.size()));
    CHECK(!headOk(KIND_ESP8266, h.data(), h.size()));
    CHECK(!headOk(KIND_ESP8266, nullptr, 0));
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

static bool validates(const void* frame, int len, uint8_t want) {
    uint8_t* buf = (uint8_t*)malloc((size_t)(len > 0 ? len : 1));
    memcpy(buf, frame, (size_t)(len > 0 ? len : 0));
    uint8_t t = 0xFF;
    const bool ok = espnowValidate(buf, len, t);
    free(buf);
    return ok && t == want;
}

static void test_fw_layouts() {
    CHECK_EQ((int)sizeof(FwGetMsg), 12);
    CHECK_EQ((int)sizeof(FwDoneMsg), 12);
    CHECK_EQ(espnowFwChunkLen(EN_FW_CHUNK_MAX), 221);
    CHECK(espnowFwChunkLen(EN_FW_CHUNK_MAX) <= ESPNOW_MAX_FRAME);
    CHECK_EQ(EN_MSG_FW_GET, 10);
    CHECK_EQ(EN_MSG_FW, 11);
    CHECK_EQ(EN_MSG_FW_DONE, 12);
    CHECK_EQ((int)EN_ACK_FW_PENDING, 4);
}

static void test_fw_get() {
    FwGetMsg g;
    espnowFillFwGet(g, 7, EN_FW_ANY, 0);
    CHECK(validates(&g, sizeof(g), EN_MSG_FW_GET));
    espnowFillFwGet(g, 7, 0xDEADBEEF, EN_FW_MAX_SIZE - 1);
    CHECK(validates(&g, sizeof(g), EN_MSG_FW_GET));
    espnowFillFwGet(g, 7, 0xDEADBEEF, EN_FW_MAX_SIZE);
    CHECK(!validates(&g, sizeof(g), EN_MSG_FW_GET));          // past any image
    espnowFillFwGet(g, 7, 1, 0);
    CHECK(!validates(&g, sizeof(g) - 1, EN_MSG_FW_GET));
}

static void test_fw_chunk() {
    uint8_t data[EN_FW_CHUNK_MAX];
    for (int i = 0; i < EN_FW_CHUNK_MAX; i++) data[i] = (uint8_t)i;
    FwChunkMsg m;

    int n = espnowFillFwChunk(m, 3, 0x11223344, 1000, 800, 3600, 2, 0, data, 200);
    CHECK_EQ(n, EN_FW_CHUNK_HDR + 200);
    CHECK(validates(&m, n, EN_MSG_FW));
    CHECK_EQ(m.minMv, 3600);
    CHECK_EQ(m.attempt, 2);
    CHECK(memcmp(m.data, data, 200) == 0);

    // The last, short slice.
    n = espnowFillFwChunk(m, 3, 0x11223344, 1000, 950, 0, 2, 0, data, 50);
    CHECK_EQ(n, EN_FW_CHUNK_HDR + 50);
    CHECK(validates(&m, n, EN_MSG_FW));
    CHECK(!validates(&m, n + 1, EN_MSG_FW));                  // length disagrees with len
                                                              // (m has room: n + 1 < sizeof)
    CHECK(!validates(&m, n - 1, EN_MSG_FW));

    // Past the end of the image.
    CHECK_EQ(espnowFillFwChunk(m, 3, 1, 1000, 951, 0, 0, 0, data, 50), -1);
    // "Nothing for you" and "busy".
    n = espnowFillFwChunk(m, 3, 0, 0, 0, 0, 0, 0, nullptr, 0);
    CHECK_EQ(n, EN_FW_CHUNK_HDR);
    CHECK(validates(&m, n, EN_MSG_FW));
    CHECK_EQ(espnowFillFwChunk(m, 3, 0, 0, 5, 0, 0, 0, nullptr, 0), -1);
    n = espnowFillFwChunk(m, 3, 9, 1000, 0, 0, 0, EN_FW_BUSY, nullptr, 0);
    CHECK(validates(&m, n, EN_MSG_FW));
    CHECK_EQ(espnowFillFwChunk(m, 3, 9, EN_FW_MAX_SIZE + 1, 0, 0, 0, 0, nullptr, 0), -1);
    CHECK_EQ(espnowFillFwChunk(m, 3, 9, 1000, 0, 0, 0, 0, nullptr, 10), -1);

    // Hand-broken frames the builder refuses to make.
    espnowFillFwChunk(m, 3, 1, 1000, 0, 0, 0, 0, data, 100);
    m.len = EN_FW_CHUNK_MAX + 1;
    CHECK(!validates(&m, (int)sizeof(m), EN_MSG_FW));
    espnowFillFwChunk(m, 3, 1, 1000, 0, 0, 0, 0, data, 100);
    m.size = 50;                                              // slice past the size
    CHECK(!validates(&m, espnowFwChunkLen(100), EN_MSG_FW));
    espnowFillFwChunk(m, 3, 1, 1000, 0, 0, 0, 0, data, 100);
    m.size = EN_FW_MAX_SIZE + 1;
    CHECK(!validates(&m, espnowFwChunkLen(100), EN_MSG_FW));
    espnowFillFwChunk(m, 3, 1, 0, 0, 0, 0, 0, nullptr, 0);
    m.offset = 4;                                             // "nothing" with an offset
    CHECK(!validates(&m, EN_FW_CHUNK_HDR, EN_MSG_FW));
    CHECK(!validates(&m, EN_FW_CHUNK_HDR - 1, EN_MSG_FW));
}

static void test_fw_done() {
    FwDoneMsg d;
    for (uint8_t st = EN_FW_ST_STAGED; st <= EN_FW_ST_LAST; st++) {
        espnowFillFwDone(d, 4, 0xABCDEF01, st, 1, 3412);
        CHECK(validates(&d, sizeof(d), EN_MSG_FW_DONE));
    }
    espnowFillFwDone(d, 4, 1, 0, 1, 0);
    CHECK(!validates(&d, sizeof(d), EN_MSG_FW_DONE));
    espnowFillFwDone(d, 4, 1, EN_FW_ST_LAST + 1, 1, 0);
    CHECK(!validates(&d, sizeof(d), EN_MSG_FW_DONE));
    espnowFillFwDone(d, 4, 1, EN_FW_ST_STAGED, 1, 0);
    uint8_t longer[sizeof(d) + 1] = {0};
    memcpy(longer, &d, sizeof(d));
    CHECK(!validates(longer, (int)sizeof(longer), EN_MSG_FW_DONE));
}

int main() {
    RUN(test_marker_text);
    RUN(test_marker_found);
    RUN(test_marker_split_everywhere);
    RUN(test_marker_absent_or_bad);
    RUN(test_marker_two);
    RUN(test_kind_names);
    RUN(test_head);
    RUN(test_fw_layouts);
    RUN(test_fw_get);
    RUN(test_fw_chunk);
    RUN(test_fw_done);
    return SUMMARY();
}
