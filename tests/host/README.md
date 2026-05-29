# Host unit tests

Fast, deterministic tests for the **platform-independent** logic in `src/`,
compiled and run on the build host with a normal `g++` toolchain — no board,
no emulator. They are the bottom of the testing pyramid: they catch logic and
concurrency regressions in milliseconds, leaving end-to-end / chaos testing
(Wokwi) for a later phase.

## What is covered

| File | Under test |
|------|------------|
| `test_sensor_types.cpp` | `SensorReading::toJsonLine()` (format, JSON escaping, truncation), `parseMode()`, `parseBucket()` |
| `test_ringbuffer.cpp` | `RingBuffer<N>` single-thread correctness: ordering, overflow, `fromTs` filter, `findLast`, `collectMetricSeries` |
| `test_ringbuffer_concurrency.cpp` | `RingBuffer<N>` SPSC acquire/release visibility (ThreadSanitizer target) |

Only header-only logic is exercised, so no firmware `.cpp` is linked.

## How it builds

`<Arduino.h>` and the `<freertos/*>` headers are satisfied by thin desktop
shims in `shims/` (types and C stdlib only — **not** a board emulation). The
shim directory is on the include path *only* for these tests and must never be
added to the firmware build.

## Run locally

```bash
# from the repo root
for f in tests/host/test_*.cpp; do
  g++ -std=gnu++17 -Wall -Wextra -O1 -g -pthread \
      -I tests/host/shims -I. "$f" -o "${f%.cpp}.bin" && "./${f%.cpp}.bin"
done
```

### With sanitizers

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
g++ -std=gnu++17 -g -pthread -fsanitize=address,undefined \
    -I tests/host/shims -I. tests/host/test_ringbuffer.cpp -o rb.bin && ./rb.bin

# ThreadSanitizer (validates the SPSC memory ordering)
g++ -std=gnu++17 -g -pthread -fsanitize=thread \
    -I tests/host/shims -I. tests/host/test_ringbuffer_concurrency.cpp -o rbc.bin && ./rbc.bin
```

CI runs all three suites under plain, ASan+UBSan, and TSan builds
(`.github/workflows/tests.yml`).
