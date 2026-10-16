// Host tests for BlockBuf (src/web/BlockBuf.h): the Kindle payload comes
// back out byte for byte, in any piece size, across block boundaries; and
// blocks come from the installed source first and go back to it.
#include <string>
#include "src/web/BlockBuf.h"
#include "check.h"

static void test_round_trip() {
    const size_t sizes[]  = { 0, 1, 511, 512, 513, 5000 };
    const size_t pieces[] = { 1, 3, 512, 1436, 9000 };
    for (size_t n : sizes) for (size_t piece : pieces) {
        std::string in;
        for (size_t i = 0; i < n; i++) in += (char)('a' + i % 26);
        BlockBuf b;
        // Written as the payload is: mostly a byte at a time, some runs.
        for (size_t i = 0; i < n; ) {
            const size_t k = (i % 7 == 0) ? (n - i < 40 ? n - i : 40) : 1;
            CHECK_EQ(b.write((const uint8_t*)in.data() + i, k), k);
            i += k;
        }
        CHECK(!b.failed());
        CHECK_EQ(b.size(), n);
        std::string out;
        uint8_t buf[9000];
        for (size_t k; (k = b.read(buf, piece)) != 0; ) {
            CHECK(k <= piece);
            out.append((const char*)buf, k);
        }
        CHECK(out == in);
    }
}

// A source with three blocks: the first three come from it, the rest from
// the heap, and every one goes back where it came from.
static uint8_t  g_pool[3][BlockBuf::BLK_BYTES];
static bool     g_used[3];
static int      g_heap = 0, g_back = 0;
static void* fakeAlloc() {
    for (int i = 0; i < 3; i++) if (!g_used[i]) { g_used[i] = true; return g_pool[i]; }
    g_heap++;
    return nullptr;
}
static bool fakeRelease(void* p) {
    for (int i = 0; i < 3; i++) if (p == g_pool[i]) { g_used[i] = false; g_back++; return true; }
    return false;
}

static void test_source() {
    BlockBufSource& src = blockBufSource();
    src.alloc = fakeAlloc; src.release = fakeRelease;
    {
        BlockBuf b;
        std::string in(BlockBuf::BLOCK * 5 + 7, 'x');
        CHECK_EQ(b.write((const uint8_t*)in.data(), in.size()), in.size());
        CHECK(g_used[0] && g_used[1] && g_used[2]);
        CHECK_EQ(g_heap, 3);                       // blocks 4, 5 and 6
        uint8_t buf[700];
        CHECK_EQ(b.read(buf, 700), (size_t)700);   // the first block is back
        CHECK_EQ(g_back, 1);
        CHECK(!g_used[0]);
    }                                              // the rest on destruction
    CHECK_EQ(g_back, 3);
    CHECK(!g_used[1] && !g_used[2]);
    src = BlockBufSource();
}

int main() {
    RUN(test_round_trip);
    RUN(test_source);
    return SUMMARY();
}
