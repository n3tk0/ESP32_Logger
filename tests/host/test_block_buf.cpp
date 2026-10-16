// Host tests for BlockBuf (src/web/BlockBuf.h): the Kindle payload comes
// back out byte for byte, in any piece size, across block boundaries.
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

int main() {
    RUN(test_round_trip);
    return SUMMARY();
}
