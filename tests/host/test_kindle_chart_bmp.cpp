// Host unit tests for src/web/KindleChartBmp.h
//
// The Kindle fetches this image over the network, so the transport decides how
// the bytes come out — and the first version of the streamer assumed it got a
// generous buffer every time. It wrote a 118-byte header into whatever it was
// handed without measuring, and when the buffer was too small for one row it
// rounded UP to a full row and wrote past the end. Both are reachable by any
// client that can make the send window small, which on wifi is every client.
//
// So the property under test is not "the picture looks right" — a host cannot
// judge that — but the two things a host CAN judge and that were wrong:
//
//   • the same bytes come out no matter how the reads are sized, down to one
//     byte at a time;
//   • nothing is ever written past the length the caller offered.
//
// The second is checked with a guard region rather than by inspection, because
// an overflow of a few bytes into a live heap is exactly the failure that does
// not show up until much later and somewhere else.
#include <stdint.h>
#include <string.h>
#include <vector>

#include "src/web/KindleChartBmp.h"
#include "check.h"

// ---------------------------------------------------------------------------
// A chart with something in it
// ---------------------------------------------------------------------------
// Flat data would render an empty frame and prove nothing about the row cache:
// every row would be identical, so serving the wrong one would pass. This gives
// each hour a different value so rows genuinely differ.
static void fillCtx(ChartBmpCtx& c, bool withIndoor) {
    c.haveOut = true;
    c.haveIn  = withIndoor;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        const float base = 5.0f + (float)i * 0.9f;
        c.tOut[i].count = 4;
        c.tOut[i].min   = base - 1.5f;
        c.tOut[i].max   = base + 1.5f;
        c.tOut[i].sum   = base * 4.0f;

        if (withIndoor) {
            c.tIn[i].count = 4;
            c.tIn[i].min   = 20.0f;
            c.tIn[i].max   = 22.0f;
            c.tIn[i].sum   = 21.0f * 4.0f;
        } else {
            c.tIn[i].count = 0;
        }
    }
}

/// Read the whole image with every read capped at `chunk` bytes, into a vector.
/// The scratch buffer is bracketed by a guard the reader must not touch.
static std::vector<uint8_t> renderWith(size_t chunk, bool withIndoor,
                                       uint16_t W, uint16_t H) {
    ChartBmpReader rd;
    fillCtx(rd.ctx, withIndoor);
    rd.ctx.init(W, H);
    rd.begin();

    static const uint8_t GUARD = 0xA5;
    const size_t PAD = 64;

    std::vector<uint8_t> outAll;
    std::vector<uint8_t> scratch(chunk + 2 * PAD);

    int spins = 0;
    while (!rd.done()) {
        memset(scratch.data(), GUARD, scratch.size());
        const size_t n = rd.read(scratch.data() + PAD, chunk);

        // Zero before the end of the image would stall the real transport:
        // AsyncWebServer treats a zero-length fill as "the body is finished",
        // so a reader that returns early truncates the picture.
        CHECK(n > 0);
        CHECK(n <= chunk);

        for (size_t i = 0; i < PAD; i++) {
            CHECK_EQ((int)scratch[i], (int)GUARD);                    // before
            CHECK_EQ((int)scratch[PAD + chunk + i], (int)GUARD);      // after
        }
        // And nothing beyond what it said it wrote, inside the window either.
        for (size_t i = n; i < chunk; i++)
            CHECK_EQ((int)scratch[PAD + i], (int)GUARD);

        outAll.insert(outAll.end(), scratch.begin() + PAD,
                                    scratch.begin() + PAD + n);

        // A reader that never finishes is a hung connection, not a slow one.
        if (++spins > 4000000) { CHECK(false); break; }
    }
    return outAll;
}

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void test_header_is_a_valid_4bit_bmp() {
    uint8_t h[KD_BMP_HEADER_SIZE];
    const size_t n = writeBmpHeader(h, 560, 200);
    CHECK_EQ((int)n, (int)KD_BMP_HEADER_SIZE);

    CHECK_EQ((int)h[0], (int)'B');
    CHECK_EQ((int)h[1], (int)'M');

    const uint32_t rowBytes  = 560 / 2;
    const uint32_t pixels    = rowBytes * 200;
    CHECK_EQ((long)le32(h + 2),  (long)(KD_BMP_HEADER_SIZE + pixels));  // file size
    CHECK_EQ((long)le32(h + 10), (long)KD_BMP_HEADER_SIZE);             // pixel offset
    CHECK_EQ((long)le32(h + 14), 40L);                                  // info header
    CHECK_EQ((long)le32(h + 18), 560L);                                 // width
    CHECK_EQ((long)le32(h + 22), 200L);                                 // height, +ve = bottom-up
    CHECK_EQ((int)le16(h + 26), 1);                                     // planes
    CHECK_EQ((int)le16(h + 28), 4);                                     // bits per pixel
    CHECK_EQ((long)le32(h + 30), 0L);                                   // BI_RGB

    // A 4-bit BMP with no palette is unreadable, and the palette must be grey:
    // B, G and R equal, ramping 0x00 to 0xFF.
    for (int i = 0; i < 16; i++) {
        const uint8_t* e = h + 54 + i * 4;
        CHECK_EQ((int)e[0], (int)e[1]);
        CHECK_EQ((int)e[1], (int)e[2]);
        CHECK_EQ((int)e[3], 0);
    }
    CHECK_EQ((int)h[54], 0x00);              // index 0 is black
    CHECK_EQ((int)h[54 + 15 * 4], 0xFF);     // index 15 is white
}

// A 4-bit row is w/2 bytes, and both widths the dashboard uses must divide
// evenly — an odd width would drop the last pixel of every row with no error.
static void test_row_bytes_are_exact() {
    ChartBmpCtx c{};
    fillCtx(c, true);

    c.init(560, 200);
    CHECK_EQ((int)c.rowBytes, 280);
    CHECK_EQ((int)(c.rowBytes * 2), 560);

    c.init(1000, 360);
    CHECK_EQ((int)c.rowBytes, 500);
    CHECK((size_t)c.rowBytes <= KD_BMP_MAX_ROW_BYTES);
}

// ---------------------------------------------------------------------------
// The property that matters
// ---------------------------------------------------------------------------
static void test_chunking_is_transparent() {
    const std::vector<uint8_t> ref = renderWith(4096, true, 560, 200);

    const uint32_t expect = (uint32_t)KD_BMP_HEADER_SIZE + 280u * 200u;
    CHECK_EQ((long)ref.size(), (long)expect);

    // 1 byte at a time is the extreme the old code could not survive: smaller
    // than the header, smaller than a row, and it exercises every split point.
    // 117 and 119 straddle the header boundary; 279 and 281 straddle a row.
    const size_t chunks[] = {1, 2, 3, 7, 64, 117, 118, 119, 279, 280, 281, 512, 1436, 65536};
    for (size_t c : chunks) {
        const std::vector<uint8_t> got = renderWith(c, true, 560, 200);
        CHECK_EQ((long)got.size(), (long)ref.size());
        CHECK(got == ref);
    }
}

// The same, at the resolution the Paperwhite uses — where a row is 500 bytes
// and a single 1436-byte send spans nearly three of them.
static void test_chunking_is_transparent_at_high_res() {
    const std::vector<uint8_t> ref = renderWith(8192, true, 1000, 360);
    CHECK_EQ((long)ref.size(), (long)(KD_BMP_HEADER_SIZE + 500u * 360u));

    const size_t chunks[] = {1, 5, 118, 499, 500, 501, 1436, 4096};
    for (size_t c : chunks) CHECK(renderWith(c, true, 1000, 360) == ref);
}

// The image says how long it is, and it has to be telling the truth.
//
// THE READER NOW BELIEVES IT. kindle/update_dash.sh refuses to replace the
// chart on screen with a download whose byte count does not match the size
// written at offset 2 of its own header — that is how it tells a whole image
// from one that stopped when the connection died, which was showing up on the
// device as a chart that vanished for a while and came back.
//
// So a header that promises a different number from what the streamer emits
// would no longer be a cosmetic mistake in a field no viewer reads: it would
// be a collector whose every chart the reader throws away, on a device with no
// way to say so. Two numbers, computed in two places (writeBmpHeader() from w
// and h, begin() from rowBytes and H), checked against a third: the bytes that
// actually came out.
static void test_the_declared_size_is_the_size_that_is_sent() {
    struct { uint16_t w, h; } sizes[] = { {560, 200}, {1000, 360} };
    for (auto& s : sizes) {
        const std::vector<uint8_t> img = renderWith(1436, true, s.w, s.h);
        CHECK_EQ((long)le32(img.data() + 2), (long)img.size());
        // …and the pixel data starts where the header says it does, or a
        // reader counting from 118 reads the palette as pixels.
        CHECK_EQ((long)le32(img.data() + 10), (long)KD_BMP_HEADER_SIZE);
    }
}

// The row cache is only correct if rows actually differ; if the renderer
// produced one repeated row, every test above would pass while serving the
// wrong row for every request.
static void test_rows_are_not_all_identical() {
    const std::vector<uint8_t> img = renderWith(4096, true, 560, 200);
    const uint8_t* px = img.data() + KD_BMP_HEADER_SIZE;

    int distinct = 0;
    for (int r = 1; r < 200; r++)
        if (memcmp(px + (size_t)r * 280, px + (size_t)(r - 1) * 280, 280) != 0)
            distinct++;
    CHECK(distinct > 5);
}

// A collector with no indoor sensor configured still has to produce a picture,
// not a division by zero or an empty body.
static void test_renders_without_indoor_data() {
    const std::vector<uint8_t> img = renderWith(1436, false, 560, 200);
    CHECK_EQ((long)img.size(), (long)(KD_BMP_HEADER_SIZE + 280u * 200u));
    CHECK(renderWith(1, false, 560, 200) == img);
}

// Nothing reported at all: every hour empty. lo/hi start at ±1e9 and stay
// there, so the span guard is the only thing between this and a scale of two
// billion — or a divide by zero.
static void test_survives_a_completely_empty_ring() {
    ChartBmpReader rd;
    rd.ctx.haveOut = false;
    rd.ctx.haveIn  = false;
    for (int i = 0; i < TrendRing::HOURS; i++) {
        rd.ctx.tOut[i].count = 0;
        rd.ctx.tIn[i].count  = 0;
    }
    rd.ctx.init(560, 200);
    rd.begin();

    CHECK(rd.ctx.span > 0.0f);
    CHECK_EQ((long)rd.total, (long)(KD_BMP_HEADER_SIZE + 280u * 200u));

    std::vector<uint8_t> buf(600);
    size_t got = 0;
    int spins = 0;
    while (!rd.done() && ++spins < 100000) got += rd.read(buf.data(), buf.size());
    CHECK_EQ((long)got, (long)rd.total);
}

int main() {
    RUN(test_header_is_a_valid_4bit_bmp);
    RUN(test_row_bytes_are_exact);
    RUN(test_chunking_is_transparent);
    RUN(test_chunking_is_transparent_at_high_res);
    RUN(test_the_declared_size_is_the_size_that_is_sent);
    RUN(test_rows_are_not_all_identical);
    RUN(test_renders_without_indoor_data);
    RUN(test_survives_a_completely_empty_ring);
    return SUMMARY();
}
