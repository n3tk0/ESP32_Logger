// Host unit tests for src/nodes/NodeFwRules.h — the collector's node
// firmware decisions, docs/NODE_OTA.md §2–§4:
//
//   • status names, the terminal states, the ACK flag, attempt wrap;
//   • key → kind;
//   • the WiFi ingest verdict (done / failed / offer / nothing);
//   • FW_DONE → status and its error text;
//   • the serving windows: a whole transfer driven through lookup()/take()/
//     loaded() byte for byte, a node that jumps, read-ahead, busy.
#include <stdint.h>
#include <string.h>
#include <vector>

#include "src/nodes/NodeFwRules.h"
#include "check.h"

using namespace nfr;

static void test_status() {
    for (uint8_t s = ST_PENDING; s < ST_COUNT; s++) CHECK_EQ(statusParse(statusName(s)), s);
    CHECK_EQ(statusParse("nope"), ST_NONE);
    CHECK_EQ(statusParse(""), ST_NONE);
    CHECK_EQ(statusParse(nullptr), ST_NONE);
    CHECK_STREQ(statusName(ST_COUNT), "");

    CHECK(!isOpen(ST_NONE));
    CHECK(!isOpen(ST_DONE));
    CHECK(!isOpen(ST_FAILED));
    CHECK(isOpen(ST_PENDING) && isOpen(ST_SENDING) && isOpen(ST_STAGED) && isOpen(ST_DEFERRED));

    // §3: WiFi offers only pending / sending; §4.1: the flag covers staged
    // and deferred too.
    CHECK(wifiOffered(ST_PENDING) && wifiOffered(ST_SENDING));
    CHECK(!wifiOffered(ST_STAGED) && !wifiOffered(ST_DONE) && !wifiOffered(ST_FAILED));
    CHECK(ackFlag(ST_STAGED) && ackFlag(ST_DEFERRED) && ackFlag(ST_PENDING));
    CHECK(!ackFlag(ST_NONE) && !ackFlag(ST_DONE) && !ackFlag(ST_FAILED));

    CHECK_EQ(nextAttempt(0), 1);
    CHECK_EQ(nextAttempt(7), 8);
    CHECK_EQ(nextAttempt(254), 255);
    CHECK_EQ(nextAttempt(255), 1);   // never 0
}

static void test_keys() {
    CHECK_EQ(kindOfKey("w:balcony"), nodefw::KIND_ESP8266);
    CHECK_EQ(kindOfKey("e:3"), nodefw::KIND_ESPNOW_C3);
    CHECK_EQ(kindOfKey("w:"), nodefw::KIND_NONE);
    CHECK_EQ(kindOfKey("x:3"), nodefw::KIND_NONE);
    CHECK_EQ(kindOfKey("e3"), nodefw::KIND_NONE);
    CHECK_EQ(kindOfKey(""), nodefw::KIND_NONE);
    CHECK_EQ(kindOfKey(nullptr), nodefw::KIND_NONE);
}

static void test_wifi() {
    const char* img = "0123456789abcdef0123456789abcdef";
    const char* old = "ffffffffffffffffffffffffffffffff";
    // Not a target, no image, a node that sends no fw_md5: nothing.
    CHECK_EQ(wifiIngest(ST_NONE, old, img, nullptr, -1, 3), W_NONE);
    CHECK_EQ(wifiIngest(ST_PENDING, old, "", nullptr, -1, 3), W_NONE);
    CHECK_EQ(wifiIngest(ST_PENDING, "", img, nullptr, -1, 3), W_NONE);
    // Offered while pending / sending.
    CHECK_EQ(wifiIngest(ST_PENDING, old, img, nullptr, -1, 3), W_OFFER);
    CHECK_EQ(wifiIngest(ST_SENDING, old, img, nullptr, -1, 3), W_OFFER);
    CHECK_EQ(wifiIngest(ST_FAILED, old, img, nullptr, -1, 3), W_NONE);
    // Running it: done from any state, once.
    CHECK_EQ(wifiIngest(ST_PENDING, img, img, nullptr, -1, 3), W_DONE);
    CHECK_EQ(wifiIngest(ST_FAILED, img, img, nullptr, -1, 3), W_DONE);
    CHECK_EQ(wifiIngest(ST_DONE, img, img, nullptr, -1, 3), W_NONE);
    // Running it beats an error it still carries.
    CHECK_EQ(wifiIngest(ST_SENDING, img, img, img, 3, 3), W_DONE);
    // An error counts for this image and attempt only.
    CHECK_EQ(wifiIngest(ST_SENDING, old, img, img, 3, 3), W_FAILED);
    CHECK_EQ(wifiIngest(ST_SENDING, old, img, img, 2, 3), W_OFFER);   // before a Retry
    CHECK_EQ(wifiIngest(ST_SENDING, old, img, old, 3, 3), W_OFFER);   // another image
    CHECK_EQ(wifiIngest(ST_FAILED, old, img, img, 3, 3), W_NONE);     // already failed
}

static void test_done() {
    // RUNNING: done from any state and any attempt, but only for our image.
    CHECK_EQ(afterDone(ST_PENDING, true, EN_FW_ST_RUNNING, 1, 5), ST_DONE);
    CHECK_EQ(afterDone(ST_FAILED, true, EN_FW_ST_RUNNING, 1, 5), ST_DONE);
    CHECK_EQ(afterDone(ST_DONE, true, EN_FW_ST_RUNNING, 5, 5), ST_NONE);
    CHECK_EQ(afterDone(ST_PENDING, false, EN_FW_ST_RUNNING, 5, 5), ST_NONE);
    CHECK_EQ(afterDone(ST_NONE, true, EN_FW_ST_RUNNING, 5, 5), ST_NONE);
    // The rest: current attempt, open target.
    CHECK_EQ(afterDone(ST_SENDING, true, EN_FW_ST_STAGED, 5, 5), ST_STAGED);
    CHECK_EQ(afterDone(ST_SENDING, true, EN_FW_ST_STAGED, 4, 5), ST_NONE);
    CHECK_EQ(afterDone(ST_PENDING, true, EN_FW_ST_LOW_BATTERY, 5, 5), ST_DEFERRED);
    CHECK_EQ(afterDone(ST_STAGED, true, EN_FW_ST_ROLLED_BACK, 5, 5), ST_FAILED);
    CHECK_EQ(afterDone(ST_SENDING, true, EN_FW_ST_BAD_IMAGE, 5, 5), ST_FAILED);
    CHECK_EQ(afterDone(ST_SENDING, true, EN_FW_ST_FLASH_ERROR, 5, 5), ST_FAILED);
    CHECK_EQ(afterDone(ST_FAILED, true, EN_FW_ST_STAGED, 5, 5), ST_NONE);
    CHECK_EQ(afterDone(ST_DONE, true, EN_FW_ST_BAD_IMAGE, 5, 5), ST_NONE);

    char e[40];
    doneError(e, sizeof(e), EN_FW_ST_LOW_BATTERY, 3410);
    CHECK_STREQ(e, "battery 3.41 V");
    doneError(e, sizeof(e), EN_FW_ST_LOW_BATTERY, 3999);
    CHECK_STREQ(e, "battery 4.00 V");
    doneError(e, sizeof(e), EN_FW_ST_LOW_BATTERY, 3004);
    CHECK_STREQ(e, "battery 3.00 V");
    doneError(e, sizeof(e), EN_FW_ST_ROLLED_BACK, 0);
    CHECK_STREQ(e, "rolled back");
    doneError(e, sizeof(e), EN_FW_ST_BAD_IMAGE, 0);
    CHECK_STREQ(e, "bad image");
    doneError(e, sizeof(e), EN_FW_ST_STAGED, 0);
    CHECK_STREQ(e, "");

    CHECK(getMeansSending(ST_PENDING) && getMeansSending(ST_DEFERRED));
    CHECK(!getMeansSending(ST_SENDING) && !getMeansSending(ST_STAGED));
}

static void test_pct() {
    CHECK_EQ(pct(0, 0), 0);
    CHECK_EQ(pct(0, 1000), 0);
    CHECK_EQ(pct(500, 1000), 50);
    CHECK_EQ(pct(999, 1000), 99);
    CHECK_EQ(pct(1000, 1000), 100);
    CHECK_EQ(pct(0x13FFFF, 0x140000), 99);   // no overflow at the largest size
}

// ---------------------------------------------------------------------------
// Serving windows
// ---------------------------------------------------------------------------

struct Rig {
    Serve                s;
    std::vector<uint8_t> img;
    uint8_t              buf[2][WIN];
    int                  loads = 0;

    explicit Rig(uint32_t size) : img(size) {
        serveReset(s);
        for (uint32_t i = 0; i < size; i++) img[i] = (uint8_t)(i * 7 + (i >> 8));
    }
    /// The loop's half, as serviceFw() runs it.
    void tick() {
        uint8_t idx; uint32_t off;
        if (!take(s, idx, off)) return;
        const uint32_t n = loadLen(off, (uint32_t)img.size());
        memcpy(buf[idx], img.data() + off, n);
        loaded(s, idx, n);
        loads++;
    }
    /// The callback's half: bytes answered for a FW_GET at `off`, or -1.
    int get(uint32_t off, std::vector<uint8_t>& out) {
        uint8_t n;
        const int w = lookup(s, off, (uint32_t)img.size(), n);
        if (w < 0) return -1;
        out.insert(out.end(), buf[w] + (off - s.w[w].base), buf[w] + (off - s.w[w].base) + n);
        return n;
    }
};

/// A node reading the image in order, asking again on no answer, the loop
/// ticking once per request. Every byte arrives, in order, and read-ahead
/// keeps the misses to the first one.
static void transfer(uint32_t size, int& misses, int& loads) {
    Rig r(size);
    std::vector<uint8_t> got;
    uint32_t off = 0;
    misses = 0;
    int guard = 0;
    while (off < size && guard++ < 100000) {
        const int n = r.get(off, got);
        if (n < 0) misses++;
        else { CHECK(n > 0 && n <= EN_FW_CHUNK_MAX); off += (uint32_t)n; }
        r.tick();
    }
    CHECK_EQ(off, size);
    CHECK(got == r.img);
    loads = r.loads;
}

static void test_serve_transfer() {
    int misses, loads;
    transfer(466848, misses, loads);
    CHECK_EQ(misses, 1);                          // only the very first request
    CHECK_EQ(loads, (466848 + WIN - 1) / WIN);    // each byte read from SD once
    transfer(1, misses, loads);
    CHECK_EQ(misses, 1);
    transfer(WIN, misses, loads);
    CHECK_EQ(loads, 1);
    transfer(WIN + 1, misses, loads);
    CHECK_EQ(loads, 2);
    transfer(0x140000, misses, loads);
    CHECK_EQ(misses, 1);
}

static void test_serve_slices() {
    Rig r(10000);
    std::vector<uint8_t> got;
    CHECK_EQ(r.get(0, got), -1);
    // Asking again before the loop ran does not ask twice.
    CHECK_EQ(r.get(0, got), -1);
    r.tick();
    CHECK_EQ(r.loads, 1);
    CHECK_EQ(r.get(0, got), EN_FW_CHUNK_MAX);
    // A slice ends at its window's end: 4000 + 96 = 4096.
    CHECK_EQ(r.get(4000, got), 96);
    // Past the middle a read-ahead was asked for, into the other window.
    CHECK(r.s.req);
    CHECK_EQ(r.s.reqOff, WIN);
    r.tick();
    CHECK_EQ(r.get(WIN, got), EN_FW_CHUNK_MAX);
    // The last slice is short and ends at the image's end.
    got.clear();
    CHECK_EQ(r.get(9990, got), -1);   // a jump: not in either window
    r.tick();
    CHECK_EQ(r.get(9990, got), 10);
    CHECK(memcmp(got.data(), r.img.data() + 9990, 10) == 0);
    // Beyond the image: never answered, nothing loaded.
    CHECK_EQ(r.get(10000, got), -1);
    CHECK(!r.s.req);
}

static void test_serve_loading() {
    Rig r(20000);
    std::vector<uint8_t> got;
    r.get(0, got);
    uint8_t idx; uint32_t off;
    CHECK(take(r.s, idx, off));
    CHECK_EQ(off, 0u);
    // While a window is being filled it answers nothing, and a request it
    // will cover asks for nothing more.
    CHECK_EQ(r.get(100, got), -1);
    CHECK(!r.s.req);
    // A second take waits for the first to be published.
    CHECK(!take(r.s, idx, off));
    memcpy(r.buf[idx], r.img.data(), WIN);
    loaded(r.s, idx, WIN);
    CHECK_EQ(r.get(100, got), EN_FW_CHUNK_MAX);
    // A failed read leaves the window empty; the next request asks again.
    r.get(15000, got);
    CHECK(take(r.s, idx, off));
    loaded(r.s, idx, 0);
    CHECK_EQ(r.get(15000, got), -1);
    CHECK(r.s.req);
    CHECK_EQ(r.s.reqOff, 15000u);
    // The window replaced on a miss is the one further behind, never the
    // one still being filled.
    serveReset(r.s);
    r.s.w[0] = { 0, WIN };
    r.s.w[1] = { WIN, WIN };
    r.get(12000, got);
    CHECK_EQ(r.s.reqIdx, 0);
    CHECK(take(r.s, idx, off));
    r.get(18000, got);
    CHECK_EQ(r.s.reqIdx, 1);
}

static void test_busy() {
    CHECK(!busyFor(0, 0, 3));              // no transfer
    CHECK(!busyFor(3, 0, 3));              // its own
    CHECK(busyFor(3, 100, 4));             // another node's, live
    CHECK(busyFor(3, IDLE_MS - 1, 4));
    CHECK(!busyFor(3, IDLE_MS, 4));        // idle: dropped
}

int main() {
    RUN(test_status);
    RUN(test_keys);
    RUN(test_wifi);
    RUN(test_done);
    RUN(test_pct);
    RUN(test_serve_transfer);
    RUN(test_serve_slices);
    RUN(test_serve_loading);
    RUN(test_busy);
    return SUMMARY();
}
