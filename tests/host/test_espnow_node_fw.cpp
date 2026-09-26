// The ESP-NOW node's side of a firmware update (docs/NODE_OTA.md §4.3, §4.4):
// the per-wake download state machine (node_espnow/src/FwFetch.h) and the
// trial / rollback decisions (node_espnow/src/FwTrial.h).
//
// WHY THIS FILE EXISTS
// --------------------
// A download spans several wakes and ~6500 slices, and what it writes ends up
// as the next boot image. None of the ways it can go wrong — a reply lost, a
// wake that runs out of budget mid-sector, an image replaced between two
// wakes, a battery that sags — shows up on a bench in the time anyone watches
// one. So the collector is simulated with the same frame builders the real
// one uses, the flash is simulated with NOR semantics (erase to 0xFF, a write
// can only clear bits, so a sector written without being erased comes out
// wrong), the clock is fake, and every download is compared byte for byte
// with the image at the end.
#include <stdint.h>
#include <string.h>

#include <vector>

#include "node_espnow/src/FwFetch.h"
#include "node_espnow/src/FwTrial.h"
#include "check.h"

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static const uint8_t  NODE     = 7;
static const uint32_t RUNNING  = 0x11111111;   ///< the image the node runs
static const uint32_t SLOT     = 0x140000;
static const uint32_t PART     = 0x150000;     ///< ota_1 on the C3's default table

static std::vector<uint8_t> makeImage(uint32_t size, uint32_t seed) {
    std::vector<uint8_t> v(size);
    uint32_t x = seed;
    for (uint32_t i = 0; i < size; i++) {
        x = x * 1103515245u + 12345u;
        v[i] = (uint8_t)(x >> 16);
    }
    return v;
}

/// The collector, as far as FW_GET is concerned.
struct SimCollector {
    std::vector<uint8_t> img;
    uint32_t imgId    = 0x22222222;
    uint8_t  attempt  = 1;
    uint16_t minMv    = 3600;
    bool     busy     = false;
    bool     cancelled = false;
    bool     silent   = false;
    int      dropEvery = 0;
    uint32_t latencyMs = 4;
    int      served   = 0;
    int      anyAsked = 0;       ///< FW_GET(ANY, …) seen
    /// The collector's RAM window (docs/NODE_OTA.md §4.2): a slice ends at
    /// the window's end, so mid-image slices can be shorter than 200. 0 = none.
    uint32_t window   = 0;
    int      anyDeaf  = 0;       ///< leave this many FW_GET(ANY) unanswered (slice 0 loading)
    int      emptyEvery = 0;     ///< every Nth slice comes back with len 0

    /// Answer one request. False = nothing came back.
    bool answer(const FwGetMsg& q, FwChunkMsg& out) {
        if (silent || q.nodeId != NODE) return false;
        served++;
        if (dropEvery && served % dropEvery == 0) return false;
        int len;
        if (busy) {
            len = espnowFillFwChunk(out, NODE, 0, 0, 0, 0, 0, EN_FW_BUSY, nullptr, 0);
        } else if (cancelled) {
            len = espnowFillFwChunk(out, NODE, 0, 0, 0, 0, 0, 0, nullptr, 0);
        } else {
            // FW_GET(ANY) and a request for another id are both answered
            // with what this collector holds — the node decides.
            if (q.imgId == EN_FW_ANY && ++anyAsked <= anyDeaf) return false;
            const uint32_t off = q.imgId == EN_FW_ANY ? 0 : q.offset;
            if (off >= img.size()) return false;
            uint32_t n = (uint32_t)img.size() - off;
            if (n > EN_FW_CHUNK_MAX) n = EN_FW_CHUNK_MAX;
            if (window && n > window - off % window) n = window - off % window;
            if (emptyEvery && off && served % emptyEvery == 0) n = 0;
            len = espnowFillFwChunk(out, NODE, imgId, (uint32_t)img.size(), off, minMv, attempt,
                                    0, img.data() + off, (uint8_t)n);
        }
        // Everything the node receives passed the validator on the air.
        uint8_t type = 0;
        CHECK(len > 0);
        CHECK(espnowValidate((const uint8_t*)&out, len, type));
        CHECK_EQ(type, EN_MSG_FW);
        return len > 0;
    }
};

/// The OTA slot, with NOR flash's rules.
struct SimFlash {
    std::vector<uint8_t> mem = std::vector<uint8_t>(SLOT, 0x00);   // not erased
    bool failWrites = false;
    int  erases     = 0;

    bool write(const FwChunkMsg& m) {
        if (failWrites) return false;
        uint32_t first = 0;
        const uint32_t n = enfw::sectorsEntered(m.offset, m.len, first);
        for (uint32_t i = 0; i < n; i++) {
            memset(mem.data() + first + i * enfw::SECTOR, 0xFF, enfw::SECTOR);
            erases++;
        }
        for (uint32_t i = 0; i < m.len; i++) mem[m.offset + i] &= m.data[i];
        return true;
    }
    bool holds(const std::vector<uint8_t>& img) const {
        return memcmp(mem.data(), img.data(), img.size()) == 0;
    }
};

/// What survives between wakes (RTC memory on the node).
struct Node {
    enfw::Progress prog;
    enfw::Failed   failed;
    uint32_t rbImg = 0;
    uint8_t  rbAtt = 0;
    uint16_t vbat  = 3900;
    uint32_t part  = PART;
    Node() {
        enfw::forget(prog);
        memset(&failed, 0, sizeof(failed));
    }
};

struct WakeResult {
    enfw::Step step;
    bool       gaveUp;
    int        requests;
    uint32_t   spentMs;
    uint32_t   firstWant;    ///< the id the wake's first FW_GET asked for
};

/// One wake's download, as FwFetch.cpp's fwOnPending() drives it, on a fake
/// clock. A slice's write costs 1 ms, an erase 25 (the C3's datasheet range).
static enfw::Fetch g_f;
static WakeResult runWake(SimCollector& col, Node& n, SimFlash& fl, uint32_t budgetMs = 20000,
                          uint32_t windowMs = 50, uint8_t maxMisses = 6) {
    WakeResult r{enfw::Step::Request, false, 0, 0, 0xFFFFFFFF};
    uint32_t now = 500000;
    enfw::begin(g_f, NODE, RUNNING, n.rbImg, n.rbAtt, n.vbat, n.part, SLOT, now, budgetMs,
                maxMisses);
    FwGetMsg q;
    static FwChunkMsg m;
    for (int guard = 0; guard < 100000; guard++) {
        if (!enfw::wantRequest(g_f, now, q)) { r.gaveUp = true; break; }
        CHECK_EQ(q.type, EN_MSG_FW_GET);
        if (r.firstWant == 0xFFFFFFFF) {
            r.firstWant = q.imgId;
            CHECK_EQ(q.offset, 0u);
        }
        uint32_t w = enfw::remainingMs(g_f, now);
        if (w > windowMs) w = windowMs;
        r.requests++;
        const bool got = col.answer(q, m) && col.latencyMs <= w;
        now += got ? col.latencyMs : w;
        enfw::Step s = enfw::onReply(g_f, n.prog, n.failed, got ? &m : nullptr);
        if (s == enfw::Step::Write) {
            const int e0 = fl.erases;
            const bool ok = fl.write(*g_f.slice);
            now += 1 + 25 * (uint32_t)(fl.erases - e0);
            s = enfw::wrote(g_f, n.prog, ok);
        }
        r.step = s;
        if (s != enfw::Step::Request) break;
    }
    r.spentMs = now - 500000;
    return r;
}

// ---------------------------------------------------------------------------
// Sectors
// ---------------------------------------------------------------------------

static void test_sectors_are_erased_as_they_are_entered() {
    uint32_t first = 0;
    CHECK_EQ(enfw::sectorsEntered(0, 200, first), 1u);        // byte 0 enters sector 0
    CHECK_EQ(first, 0u);
    CHECK_EQ(enfw::sectorsEntered(200, 200, first), 0u);      // mid-sector: already erased
    CHECK_EQ(enfw::sectorsEntered(4000, 200, first), 1u);     // crosses into sector 1
    CHECK_EQ(first, 4096u);
    CHECK_EQ(enfw::sectorsEntered(4096, 200, first), 1u);     // starts exactly on it
    CHECK_EQ(first, 4096u);
    CHECK_EQ(enfw::sectorsEntered(3896, 200, first), 0u);     // ends exactly before it
    CHECK_EQ(enfw::sectorsEntered(4095, 1, first), 0u);
    CHECK_EQ(enfw::sectorsEntered(4095, 2, first), 1u);
    CHECK_EQ(enfw::sectorsEntered(100, 0, first), 0u);
    CHECK_EQ(enfw::sectorsEntered(0, 3 * 4096 + 1, first), 4u);
}

// ---------------------------------------------------------------------------
// The first answer decides
// ---------------------------------------------------------------------------

static void test_a_whole_image_in_one_wake_is_byte_exact() {
    SimCollector col;
    col.img = makeImage(30000, 1);
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK_EQ(r.firstWant, EN_FW_ANY);
    CHECK_EQ(n.prog.imgId, col.imgId);
    CHECK_EQ(n.prog.size, 30000u);
    CHECK_EQ(n.prog.written, 30000u);
    CHECK_EQ(n.prog.attempt, 1);
    CHECK_EQ(n.prog.partAddr, PART);
    CHECK(fl.holds(col.img));
    CHECK_EQ(fl.erases, 8);                                   // ceil(30000 / 4096)
    CHECK_EQ(r.requests, (30000 + 199) / 200);                // slice 0 came with the answer
}

static void test_running_that_image_is_reported_not_downloaded() {
    SimCollector col;
    col.img   = makeImage(5000, 2);
    col.imgId = RUNNING;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_RUNNING);
    CHECK_EQ(g_f.imgId, RUNNING);
    CHECK_EQ(r.requests, 1);
    CHECK_EQ(fl.erases, 0);
}

static void test_the_image_rolled_back_from_is_refused_for_that_attempt_only() {
    SimCollector col;
    col.img = makeImage(5000, 3);
    Node n;
    n.rbImg = col.imgId;
    n.rbAtt = 1;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_ROLLED_BACK);
    CHECK_EQ(g_f.attempt, 1);
    CHECK_EQ(fl.erases, 0);

    col.attempt = 2;                                          // the user's Retry
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
}

static void test_a_low_battery_defers_but_an_unmeasured_one_does_not() {
    SimCollector col;
    col.img = makeImage(5000, 4);
    Node n;
    n.vbat = 3500;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_LOW_BATTERY);
    CHECK_EQ(g_f.value, 3500);
    CHECK_EQ(fl.erases, 0);

    n.vbat = 0;                                               // no divider fitted
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);

    Node m;
    m.vbat    = 3500;
    col.minMv = 0;                                            // no floor set
    SimFlash fl2;
    r = runWake(col, m, fl2);
    CHECK(r.step == enfw::Step::Complete);

    Node k;
    k.vbat    = 3600;                                         // exactly at the floor
    col.minMv = 3600;
    SimFlash fl3;
    r = runWake(col, k, fl3);
    CHECK(r.step == enfw::Step::Complete);
}

static void test_busy_stops_and_keeps_the_progress() {
    SimCollector col;
    col.img = makeImage(20000, 5);
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl, 300);                  // part of it
    CHECK(r.gaveUp);
    const uint32_t had = n.prog.written;
    CHECK(had > 0 && had < 20000);

    col.busy = true;
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Busy);
    CHECK_EQ(r.requests, 1);
    CHECK_EQ(n.prog.written, had);                            // kept for later

    col.busy = false;
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
}

static void test_nothing_for_you_forgets_a_partial_download() {
    SimCollector col;
    col.img = makeImage(20000, 6);
    Node n;
    SimFlash fl;
    runWake(col, n, fl, 300);
    CHECK(n.prog.written > 0);

    col.cancelled = true;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Nothing);
    CHECK_EQ(n.prog.imgId, 0u);
    CHECK_EQ(n.prog.written, 0u);

    // Offered again later: from byte 0, and still byte-exact.
    col.cancelled = false;
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
}

static void test_a_failed_image_is_not_downloaded_again_for_that_attempt() {
    SimCollector col;
    col.img = makeImage(5000, 7);
    Node n;
    n.failed = enfw::Failed{col.imgId, 1, EN_FW_ST_BAD_IMAGE};
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_BAD_IMAGE);                 // the same verdict, again
    CHECK_EQ(r.requests, 1);
    CHECK_EQ(fl.erases, 0);

    col.attempt = 2;
    r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
}

static void test_an_image_that_cannot_fit_or_has_no_id_is_refused_before_a_write() {
    SimCollector col;
    col.img = makeImage(5000, 8);
    Node n;
    SimFlash fl;
    enfw::begin(g_f, NODE, RUNNING, 0, 0, 3900, PART, 4096, 0, 1000, 6);   // a tiny slot
    FwGetMsg q;
    FwChunkMsg m;
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK(col.answer(q, m));
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_BAD_IMAGE);

    col.imgId = EN_FW_ANY;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_BAD_IMAGE);
    CHECK_EQ(fl.erases, 0);
}

// ---------------------------------------------------------------------------
// Across wakes
// ---------------------------------------------------------------------------

static void test_a_download_spans_wakes_and_resumes_where_it_stopped() {
    SimCollector col;
    col.img = makeImage(100000, 9);
    Node n;
    SimFlash fl;
    int wakes = 0;
    uint32_t last = 0;
    WakeResult r;
    do {
        r = runWake(col, n, fl, 1000);
        CHECK(r.spentMs <= 1000 + 50);                        // THE ceiling (+ one write)
        CHECK_EQ(r.firstWant, EN_FW_ANY);                     // every wake asks first
        CHECK(n.prog.written > last || r.step == enfw::Step::Complete);
        last = n.prog.written;
        wakes++;
    } while (r.step != enfw::Step::Complete && wakes < 50);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(wakes > 2);
    CHECK(fl.holds(col.img));
    CHECK_EQ(fl.erases, 25);                                  // each sector once, resumes included
}

static void test_a_wake_that_finds_every_byte_already_written_goes_straight_to_verify() {
    SimCollector col;
    col.img = makeImage(3000, 10);
    Node n;
    SimFlash fl;
    CHECK(runWake(col, n, fl).step == enfw::Step::Complete);
    // The verification never ran (a reset in between): the next offer resumes
    // at `written == size`, which is complete without another slice written.
    const int e = fl.erases;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK_EQ(r.requests, 1);
    CHECK_EQ(fl.erases, e);
}

static void test_a_new_image_between_wakes_starts_over() {
    SimCollector col;
    col.img = makeImage(20000, 11);
    Node n;
    SimFlash fl;
    runWake(col, n, fl, 300);
    CHECK(n.prog.written > 0);

    col.img   = makeImage(24000, 12);
    col.imgId = 0x33333333;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK_EQ(n.prog.imgId, 0x33333333u);
    CHECK(fl.holds(col.img));
}

static void test_a_new_attempt_of_the_same_image_starts_over() {
    SimCollector col;
    col.img = makeImage(20000, 13);
    Node n;
    SimFlash fl;
    runWake(col, n, fl, 300);
    const uint32_t had = n.prog.written;
    CHECK(had > 0);

    col.attempt = 2;
    enfw::begin(g_f, NODE, RUNNING, 0, 0, 3900, PART, SLOT, 0, 1000, 6);
    FwGetMsg q;
    FwChunkMsg m;
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK(col.answer(q, m));
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Write);
    CHECK_EQ(n.prog.written, 0u);                             // from byte 0
    CHECK_EQ(n.prog.attempt, 2);
}

static void test_another_partition_is_not_resumed_into() {
    SimCollector col;
    col.img = makeImage(20000, 14);
    Node n;
    SimFlash fl;
    runWake(col, n, fl, 300);
    CHECK(n.prog.written > 0);
    n.part = 0x10000;                                         // the slots swapped
    enfw::begin(g_f, NODE, RUNNING, 0, 0, 3900, n.part, SLOT, 0, 1000, 6);
    FwGetMsg q;
    FwChunkMsg m;
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK(col.answer(q, m));
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Write);
    CHECK_EQ(n.prog.written, 0u);
    CHECK_EQ(n.prog.partAddr, 0x10000u);
}

static void test_an_image_replaced_mid_wake_is_asked_about_again() {
    SimCollector col;
    col.img = makeImage(20000, 15);
    Node n;
    SimFlash fl;
    enfw::begin(g_f, NODE, RUNNING, 0, 0, 3900, PART, SLOT, 0, 100000, 6);
    FwGetMsg q;
    static FwChunkMsg m;
    // Ten slices of the first image…
    for (int i = 0; i < 10; i++) {
        CHECK(enfw::wantRequest(g_f, 0, q));
        CHECK(col.answer(q, m));
        CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Write);
        CHECK(fl.write(*g_f.slice));
        CHECK(enfw::wrote(g_f, n.prog, true) == enfw::Step::Request);
    }
    // …then the user uploads another. The next reply names it: not written,
    // the node asks what there is, and starts that one from 0.
    col.img   = makeImage(9000, 16);
    col.imgId = 0x44444444;
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK(col.answer(q, m));
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Request);
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK_EQ(q.imgId, EN_FW_ANY);
    CHECK_EQ(q.offset, 0u);
    for (int guard = 0; guard < 1000; guard++) {
        CHECK(col.answer(q, m));
        enfw::Step s = enfw::onReply(g_f, n.prog, n.failed, &m);
        if (s == enfw::Step::Write) {
            CHECK(fl.write(*g_f.slice));
            s = enfw::wrote(g_f, n.prog, true);
        }
        if (s == enfw::Step::Complete) break;
        CHECK(s == enfw::Step::Request);
        CHECK(enfw::wantRequest(g_f, 0, q));
    }
    CHECK_EQ(n.prog.imgId, 0x44444444u);
    CHECK_EQ(n.prog.written, 9000u);
    CHECK(fl.holds(col.img));
}

// ---------------------------------------------------------------------------
// The air
// ---------------------------------------------------------------------------

static void test_lost_replies_are_asked_for_again() {
    SimCollector col;
    col.img       = makeImage(30000, 17);
    col.dropEvery = 3;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
}

static void test_short_slices_at_the_window_edge_are_followed_by_their_length() {
    // A 4 KB window is not a multiple of 200: every twentieth slice is 96
    // bytes, and the one after it must be asked for at offset + 96.
    SimCollector col;
    col.img    = makeImage(50000, 21);
    col.window = 4096;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
    CHECK(r.requests > (50000 + 199) / 200);                  // more slices than 200-byte steps
    // Odd windows too, across wakes.
    SimCollector odd;
    odd.img    = makeImage(20011, 22);
    odd.window = 333;
    Node m;
    SimFlash fl2;
    int wakes = 0;
    do { r = runWake(odd, m, fl2, 700); } while (r.step != enfw::Step::Complete && ++wakes < 50);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl2.holds(odd.img));
}

static void test_an_empty_slice_is_a_miss_not_progress() {
    SimCollector col;
    col.img        = makeImage(20000, 23);
    col.emptyEvery = 5;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK(fl.holds(col.img));
}

static void test_the_first_request_may_go_unanswered_while_slice_0_loads() {
    SimCollector col;
    col.img     = makeImage(8000, 24);
    col.anyDeaf = 2;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Complete);
    CHECK_EQ(col.anyAsked, 3);
    CHECK(fl.holds(col.img));
}

static void test_a_silent_collector_costs_max_misses_windows() {
    SimCollector col;
    col.img    = makeImage(30000, 18);
    col.silent = true;
    Node n;
    SimFlash fl;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.gaveUp);
    CHECK_EQ(r.requests, 6);
    CHECK_EQ(r.spentMs, 300u);                                // six 50 ms windows
    CHECK_EQ(fl.erases, 0);
}

static void test_a_slice_at_the_wrong_offset_is_never_written() {
    SimCollector col;
    col.img = makeImage(5000, 19);
    Node n;
    SimFlash fl;
    enfw::begin(g_f, NODE, RUNNING, 0, 0, 3900, PART, SLOT, 0, 1000, 6);
    FwGetMsg q;
    FwChunkMsg m;
    CHECK(enfw::wantRequest(g_f, 0, q));
    CHECK(col.answer(q, m));
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Write);
    CHECK(enfw::wrote(g_f, n.prog, true) == enfw::Step::Request);
    // A late duplicate of slice 0 answering the request for 200.
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &m) == enfw::Step::Request);
    CHECK_EQ(n.prog.written, 200u);
    CHECK_EQ(g_f.misses, 1);
    // Another node's frame, or none, is a miss too.
    FwChunkMsg other = m;
    other.nodeId = NODE + 1;
    CHECK(enfw::onReply(g_f, n.prog, n.failed, &other) == enfw::Step::Request);
    CHECK(enfw::onReply(g_f, n.prog, n.failed, nullptr) == enfw::Step::Request);
    CHECK_EQ(g_f.misses, 3);
}

static void test_a_flash_error_is_reported_for_that_image() {
    SimCollector col;
    col.img = makeImage(5000, 20);
    col.attempt = 4;
    Node n;
    SimFlash fl;
    fl.failWrites = true;
    WakeResult r = runWake(col, n, fl);
    CHECK(r.step == enfw::Step::Report);
    CHECK_EQ(g_f.status, EN_FW_ST_FLASH_ERROR);
    CHECK_EQ(g_f.imgId, col.imgId);
    CHECK_EQ(g_f.attempt, 4);
}

// ---------------------------------------------------------------------------
// The trial
// ---------------------------------------------------------------------------

static void test_no_trial_decides_nothing() {
    enfwtrial::Trial t{};
    uint8_t misses = 0;
    CHECK(enfwtrial::onBoot(t, 0x10000) == enfwtrial::Boot::None);
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::None);
    CHECK_EQ(misses, 0);
}

static void test_three_starts_without_an_ack_roll_back_on_the_fourth() {
    enfwtrial::Trial t = enfwtrial::arm(0x10000, 0xAB, 3);
    CHECK(enfwtrial::armed(t));
    CHECK(enfwtrial::onBoot(t, 0x150000) == enfwtrial::Boot::Counted);
    CHECK(enfwtrial::onBoot(t, 0x150000) == enfwtrial::Boot::Counted);
    CHECK(enfwtrial::onBoot(t, 0x150000) == enfwtrial::Boot::Counted);
    CHECK_EQ(t.boots, 3);
    CHECK(enfwtrial::onBoot(t, 0x150000) == enfwtrial::Boot::RollBack);
}

static void test_running_from_prev_means_the_bootloader_went_back() {
    enfwtrial::Trial t = enfwtrial::arm(0x10000, 0xAB, 3);
    CHECK(enfwtrial::onBoot(t, 0x10000) == enfwtrial::Boot::WentBack);
    CHECK_EQ(t.boots, 0);
}

static void test_the_first_ack_confirms() {
    enfwtrial::Trial t = enfwtrial::arm(0x10000, 0xAB, 3);
    uint8_t misses = 0;
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::Counted);
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::Counted);
    CHECK(enfwtrial::onWake(t, true, misses) == enfwtrial::Wake::Confirm);
    CHECK_EQ(misses, 0);
}

static void test_mains_mode_rolls_back_on_three_unanswered_wakes() {
    // One boot, then wakes from loop(): `boots` never moves past 1.
    enfwtrial::Trial t = enfwtrial::arm(0x10000, 0xAB, 3);
    CHECK(enfwtrial::onBoot(t, 0x150000) == enfwtrial::Boot::Counted);
    uint8_t misses = 0;
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::Counted);
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::Counted);
    CHECK(enfwtrial::onWake(t, false, misses) == enfwtrial::Wake::RollBack);
}

int main() {
    RUN(test_sectors_are_erased_as_they_are_entered);
    RUN(test_a_whole_image_in_one_wake_is_byte_exact);
    RUN(test_running_that_image_is_reported_not_downloaded);
    RUN(test_the_image_rolled_back_from_is_refused_for_that_attempt_only);
    RUN(test_a_low_battery_defers_but_an_unmeasured_one_does_not);
    RUN(test_busy_stops_and_keeps_the_progress);
    RUN(test_nothing_for_you_forgets_a_partial_download);
    RUN(test_a_failed_image_is_not_downloaded_again_for_that_attempt);
    RUN(test_an_image_that_cannot_fit_or_has_no_id_is_refused_before_a_write);
    RUN(test_a_download_spans_wakes_and_resumes_where_it_stopped);
    RUN(test_a_wake_that_finds_every_byte_already_written_goes_straight_to_verify);
    RUN(test_a_new_image_between_wakes_starts_over);
    RUN(test_a_new_attempt_of_the_same_image_starts_over);
    RUN(test_another_partition_is_not_resumed_into);
    RUN(test_an_image_replaced_mid_wake_is_asked_about_again);
    RUN(test_lost_replies_are_asked_for_again);
    RUN(test_short_slices_at_the_window_edge_are_followed_by_their_length);
    RUN(test_an_empty_slice_is_a_miss_not_progress);
    RUN(test_the_first_request_may_go_unanswered_while_slice_0_loads);
    RUN(test_a_silent_collector_costs_max_misses_windows);
    RUN(test_a_slice_at_the_wrong_offset_is_never_written);
    RUN(test_a_flash_error_is_reported_for_that_image);
    RUN(test_no_trial_decides_nothing);
    RUN(test_three_starts_without_an_ack_roll_back_on_the_fourth);
    RUN(test_running_from_prev_means_the_bootloader_went_back);
    RUN(test_the_first_ack_confirms);
    RUN(test_mains_mode_rolls_back_on_three_unanswered_wakes);
    return SUMMARY();
}
