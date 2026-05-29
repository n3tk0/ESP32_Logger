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
| `test_aggregation.cpp` | `AggregationEngine` — `lttb()` (endpoint preservation, bounds), `bucket()` (raw/avg/min/max/sum), `aggregate()` pipeline bounds |
| `test_pathutils.cpp` | security-critical `Utils.cpp` helpers — `sanitizePath`, `sanitizeFilename`, `isPathProtected`, `buildPath`, `urlEncode` |

Fuzz targets (random-input property checks):

| File | Target / invariants |
|------|---------------------|
| `fuzz_pathutils.cpp` | `sanitizePath`/`sanitizeFilename`: output is rooted, contains no `..` / `//` / `\` / control bytes, and `sanitizePath` is idempotent |
| `fuzz_aggregation.cpp` | `aggregate()`: output count never exceeds `outMaxLen` / `maxPoints` (ASan/UBSan catch internal OOB / UB) |

Some suites (`test_aggregation`, `test_pathutils`, the fuzz targets) `#include` the
`.cpp` under test directly so each stays a single self-contained binary; the few
firmware globals they don't exercise (`Serial`, `usbCdc`) are stubbed in the
test TU. The rest exercise header-only logic.

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

### Fuzzing

The `fuzz_*.cpp` files run two ways:

```bash
# Seeded driver under g++ + ASan/UBSan (what CI runs — deterministic, no extra deps)
g++ -std=gnu++17 -g -O1 -DFUZZ_STANDALONE -fsanitize=address,undefined \
    -I tests/host/shims -I. tests/host/fuzz_pathutils.cpp -o fz && ./fz

# Coverage-guided libFuzzer (opt-in; needs clang + compiler-rt fuzzer runtime)
clang++ -std=gnu++17 -g -O1 -fsanitize=fuzzer,address,undefined \
    -I tests/host/shims -I. tests/host/fuzz_pathutils.cpp -o lf && ./lf -max_total_time=30
```

CI (`.github/workflows/tests.yml`) runs every `test_*` suite under plain,
ASan+UBSan and TSan, the seeded fuzz drivers under ASan+UBSan, and a
report-only cppcheck pass.
