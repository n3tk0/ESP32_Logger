// Concurrency test for the SPSC RingBuffer in src/pipeline/DataPipeline.h.
//
// One producer thread pushes, one consumer thread reads concurrently.  This is
// the ThreadSanitizer target: it exercises the acquire/release ordering between
// push() (writes slot, then releases _head) and copyRecent() (acquires _head,
// then reads the slot).  Under -fsanitize=thread a data race or a missing
// fence on the published path shows up as a TSan report (job failure).
//
// The test stays within capacity (total pushes <= N) so no slot is overwritten
// while still readable — i.e. it validates the documented SPSC contract for
// published entries, not the lossy overwrite-while-full behaviour.
#include "src/pipeline/DataPipeline.h"
#include "check.h"

#include <atomic>
#include <thread>

static SensorReading mk(float value, uint32_t ts) {
    return SensorReading::make(ts, "s", "t", "m", value, "u", QUALITY_GOOD);
}

static void test_spsc_publish_visibility() {
    constexpr int N = 256;
    RingBuffer rb;
    // Internal-RAM path: begin() must run before either thread starts, since
    // it is explicitly documented as unsafe to call alongside producers.
    CHECK(rb.begin(N, /*preferPsram=*/false));

    std::atomic<bool> producerDone{false};
    std::atomic<int>  mismatches{0};   // torn / stale reads seen by consumer

    std::thread producer([&] {
        for (int i = 0; i < N; i++) rb.push(mk((float)i, (uint32_t)i));
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        SensorReading out[N];
        // Spin reading snapshots while the producer runs.  Every PUBLISHED
        // entry must be internally consistent (we encoded ts == value), or the
        // acquire/release handshake failed to make the slot write visible.
        for (;;) {
            size_t n = rb.copyRecent(out, N);
            for (size_t k = 0; k < n; k++)
                if (out[k].timestamp != (uint32_t)out[k].value)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            if (producerDone.load(std::memory_order_acquire) && n == (size_t)N) break;
            std::this_thread::yield();   // avoid starving the producer on
                                         // single-core / loaded CI runners
        }
    });

    producer.join();
    consumer.join();

    CHECK_EQ(mismatches.load(), 0);

    // Final snapshot must contain every item in order.
    SensorReading out[N];
    size_t n = rb.copyRecent(out, N);
    CHECK_EQ(n, (size_t)N);
    bool ok = true;
    for (int i = 0; i < N; i++)
        if (out[i].value != (float)i || out[i].timestamp != (uint32_t)i) ok = false;
    CHECK(ok);
}

int main() {
    RUN(test_spsc_publish_visibility);
    return SUMMARY();
}
