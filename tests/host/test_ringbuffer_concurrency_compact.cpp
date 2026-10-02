// Concurrency test for the compact RingBuffer storage (RING_COMPACT=1, the
// ESP32-C3 layout): one producer pushes, one consumer reads at the same time,
// under ThreadSanitizer in CI.
//
// Unlike test_ringbuffer_concurrency.cpp the producer rotates through several
// metrics, so key-table entries are created while the consumer is reading.
// Each reading encodes its metric in its value, and the consumer checks the
// strings it gets back against it: a table entry written after _head is
// published would show up as a mismatch (or a TSan report).
//
// Pushes stay within capacity, so nothing is overwritten while readable — the
// documented SPSC contract for published entries.
#define RING_COMPACT 1
#include "src/pipeline/DataPipeline.h"
#include "check.h"

#include <atomic>
#include <stdio.h>
#include <thread>

static constexpr int METRICS = 12;

static void metricName(int i, char (&out)[16]) {
    snprintf(out, sizeof(out), "m%d", i % METRICS);
}

static void test_spsc_key_visibility() {
    constexpr int N = 256;
    RingBuffer rb;
    CHECK(rb.begin(N, /*preferPsram=*/false));

    std::atomic<bool> producerDone{false};
    std::atomic<int>  mismatches{0};

    std::thread producer([&] {
        char m[16];
        for (int i = 0; i < N; i++) {
            metricName(i, m);
            rb.push(SensorReading::make((uint32_t)i, "node", "t", m, (float)i, "u"));
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        SensorReading out[N];
        char m[16];
        for (;;) {
            size_t n = rb.copyRecent(out, N);
            for (size_t k = 0; k < n; k++) {
                metricName((int)out[k].value, m);
                if (out[k].timestamp != (uint32_t)out[k].value ||
                    strcmp(out[k].metric, m) != 0 ||
                    strcmp(out[k].sensorId, "node") != 0)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            }
            if (producerDone.load(std::memory_order_acquire) && n == (size_t)N) break;
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    CHECK_EQ(mismatches.load(), 0);
    CHECK_EQ(rb.keyEvictions(), (uint32_t)0);
}

int main() {
    RUN(test_spsc_key_visibility);
    return SUMMARY();
}
