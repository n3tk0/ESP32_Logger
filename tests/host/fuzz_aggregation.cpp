// Fuzz target for AggregationEngine::aggregate() (and the lttb/bucket passes it
// drives).  Derives a random reading array + parameters from the input bytes
// and asserts the output stays within bounds.  ASan/UBSan catch any internal
// out-of-bounds or UB in the LTTB / bucketing index arithmetic.
//
// Run under libFuzzer+ASan in CI; -DFUZZ_STANDALONE gives a seeded g++ driver.
#include "src/pipeline/AggregationEngine.cpp"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#define FUZZ_CHECK(cond, msg) do { if (!(cond)) {                              \
    fprintf(stderr, "INVARIANT VIOLATED: %s\n", msg); abort(); } } while (0)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 8) return 0;

    // --- derive parameters from the first bytes ---
    const AggMode    modes[]   = { AGG_RAW, AGG_AVG, AGG_MIN, AGG_MAX, AGG_LTTB, AGG_SUM };
    const TimeBucket buckets[] = { BUCKET_RAW, BUCKET_1MIN, BUCKET_5MIN, BUCKET_1HOUR, BUCKET_1DAY };
    AggMode    mode   = modes[data[0] % 6];
    TimeBucket bucket = buckets[data[1] % 5];

    constexpr size_t CAP = 256;
    size_t outMax    = 1 + (data[2] % CAP);              // 1..256
    size_t maxPoints = 1 + (data[3] % outMax);           // 1..outMax (valid contract)

    // --- build the input reading array from the remaining bytes ---
    const uint8_t* p = data + 4;
    size_t avail = size - 4;
    size_t inLen = avail / 2;
    if (inLen > CAP) inLen = CAP;

    SensorReading in[CAP];   // stack-local: keeps the harness thread-safe for
                             // parallel libFuzzer jobs
    uint32_t ts = 0;
    for (size_t i = 0; i < inLen; i++) {
        // Monotonic-ish timestamps with fuzz-controlled gaps; values from bytes.
        ts += (uint32_t)(p[(i * 2) % avail]);
        float v = (float)((int)p[(i * 2 + 1) % avail] - 128);
        in[i] = SensorReading::make(ts, "s", "t", "m", v, "u", QUALITY_GOOD);
    }

    SensorReading out[CAP];
    size_t n = AggregationEngine::aggregate(in, inLen, out, outMax,
                                            bucket, mode, maxPoints);

    FUZZ_CHECK(n <= outMax, "aggregate() returned more than outMaxLen");
    // LTTB must honour the downsampling cap directly (assert the real bound,
    // not a tautology that outMax already guarantees).
    if (mode == AGG_LTTB) FUZZ_CHECK(n <= maxPoints, "LTTB result exceeded maxPoints");
    return 0;
}

#ifdef FUZZ_STANDALONE
int main() {
    srand(20260529u);
    uint8_t buf[600];
    for (int it = 0; it < 200000; it++) {
        size_t n = (size_t)(rand() % (int)sizeof(buf));
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() & 0xff);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("standalone fuzz: 200000 iterations, all invariants held\n");
    return 0;
}
#endif
