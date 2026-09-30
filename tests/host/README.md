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
| `test_wifi_tx_power.cpp` | `WifiTxPower.h`: which powers the setting accepts, and the board default (8.5 dBm on the C3 Pico and Super Mini) |
| `test_cpu_freq.cpp` | `CpuFreq.h`: which CPU speeds the settings accept (240 MHz only on an S3), and the 160 MHz default while the web server is up |
| `test_ringbuffer.cpp` | `RingBuffer<N>` single-thread correctness: ordering, overflow, `fromTs` filter, `findLast`, `collectMetricSeries`, `copyMatching`, `latestPerMetric` |
| `test_ringbuffer_concurrency.cpp` | `RingBuffer<N>` SPSC acquire/release visibility (ThreadSanitizer target) |
| `test_aggregation.cpp` | `AggregationEngine` — `lttb()` (endpoint preservation, bounds), `bucket()` (raw/avg/min/max/sum), `aggregate()` pipeline bounds |
| `test_pathutils.cpp` | security-critical `Utils.cpp` helpers — `sanitizePath`, `sanitizeFilename`, `isPathProtected`, `buildPath`, `urlEncode` |
| `test_ringbuffer_concurrency.cpp` | the SPSC acquire/release ordering, under ThreadSanitizer |
| `test_psychrometrics.cpp` | Magnus saturation pressure, `dewPointC()`, the self-heating correction and its invariance |
| `test_httpexporter_bufsize.cpp` | the JSON body buffer-size arithmetic, extracted because the exporter cannot be compiled on the host |
| `test_refresh_cadence.cpp` | `RefreshCadence.h` — the clock's minute-boundary demand and the data prediction's age bands |
| `test_espnow_proto.cpp` | the wire format: layouts, sentinels, encode/validate round trips; the config slices, CFG_ACK, reassembly and DATA2 |
| `test_nodecfg.cpp` | `src/nodecfg/` — every validation rule of docs/NODE_CONFIG.md §1.2 one at a time, the JSON codec (round trip, partial documents, secrets), legacy `/config.json` migration, the metric catalogue, `caps` |
| `test_hw_pins.cpp` | `src/nodecfg/HwPins.h` — both chips' pin tables, label resolution, and pin-for-pin agreement with `node/src/NodePins.h` |
| `test_udp_discovery.cpp` | `src/nodecfg/UdpDiscovery.h` — the collector-discovery packets, against a reference HMAC-SHA256 checked with RFC 4231 |
| `test_nodecfg_collector.cpp` | `src/nodes/NodeCfgRules.h` — the collector's node-config decisions: revs and status, keys and file names, report adoption, the ingest reply, which secrets travel, first-contact identity, the handover, DATA2 naming |
| `test_node_fw.cpp` | `src/nodecfg/FwImage.h` and the FW_GET / FW / FW_DONE frames (docs/NODE_OTA.md §1, §4.1): the marker scanner split at every byte, two markers, the head checks and the C3 image id; frame layouts, build and validation |
| `test_node_fw_rules.cpp` | `src/nodes/NodeFwRules.h` — the collector's node-firmware decisions (docs/NODE_OTA.md): target status and the ACK flag, the WiFi offer, FW_DONE outcomes, and the two serving windows driven through whole transfers byte for byte |
| `test_espnow_node_fw.cpp` | the ESP-NOW node's firmware update (docs/NODE_OTA.md §4.3–4.4): `node_espnow/src/FwFetch.h` against a simulated collector and NOR flash — the first answer's verdicts (running, rolled back, low battery, refused before), resuming across wakes, sectors erased as entered, a new image or attempt starting over, lost replies, the budget — and `FwTrial.h`'s boot / wake counting |
| `test_node_fw_offer.cpp` | `node/src/FwOffer.h` — the WiFi node's side of docs/NODE_OTA.md §3: which `fw` offer to act on, the failed `(md5, attempt)` not retried, `fw_error` held until a POST carrying it is answered, the verdict on a whole image |
| `test_ipv4_parse.cpp` | `src/utils/Ipv4Parse.h` — the dotted-quad parser that replaced `sscanf` (300 is refused, not wrapped to 44) |
| `test_json_enum.cpp` | `src/utils/JsonEnum.h` — a module's enum field read as a number or a numeric string (the Modules form used to post `"2"`) |
| `test_espnow_nodetable.cpp` | the three decisions the collector makes about an arriving frame |
| `test_remote_ingest.cpp` | the mailbox and the separate historical queue it grew |
| `test_battery_model.cpp` | that the remaining-life model **refuses** to answer when it cannot |
| `test_datalog_format.cpp` | `src/storage/DatalogFormat.cpp` — the original data_log row byte for byte in every format, the header line and field positions, a column's aggregation after its label (`Gust[max]`) and the widest header, sensor columns with empty fields and the empty tail cut off |
| `test_live_aggregator.cpp` | `src/pipeline/LiveAggregator.cpp` — each data log sensor column by its own aggregation (avg, min, max, last, sum), no mode meaning average, empty and bad readings, the value by the mode at the close, driven as StorageTask drives it, the aggregation names and their fallback |
| `test_kindle_skin.cpp` | `KindleSkin.h` — the override CSS, the time/date/pressure formats, and the clamp that stands between a stored byte and a stylesheet |
| `test_posix_tz.cpp` | `src/utils/PosixTz.h` — the TZ string for each daylight-saving rule, the EU and US change instants as the C library reads them, and `tzOffsetAt()` across day and year boundaries |
| `test_settings_json.cpp` | `src/web/SettingsJson.cpp` — the settings file: export → import → export gives the same file, a backup restores every setting it carries (the Kindle language and page shape included) and leaves the WiFi credentials, pins and current log file alone, a key an older file lacks changes nothing, and a file gets the forms' checks. Compiles the four modules it goes through, with shims for `<WiFi.h>`, `<ESPAsyncWebServer.h>` and `<pgmspace.h>` |

Fuzz targets (random-input property checks):

| File | Target / invariants |
|------|---------------------|
| `fuzz_pathutils.cpp` | `sanitizePath`/`sanitizeFilename`: output is rooted, contains no `..` / `//` / `\` / control bytes, and `sanitizePath` is idempotent |
| `fuzz_aggregation.cpp` | `aggregate()`: output count never exceeds `outMaxLen` / `maxPoints` (ASan/UBSan catch internal OOB / UB) |

Some suites (`test_aggregation`, `test_pathutils`, `test_settings_json`, the fuzz
targets) `#include` the `.cpp` under test directly so each stays a single self-contained binary; the few
firmware globals they don't exercise (`Serial`, `usbCdc`) are stubbed in the
test TU. The rest exercise header-only logic.

## How it builds

`<Arduino.h>` and the `<freertos/*>` headers are satisfied by thin desktop
shims in `shims/` (types and C stdlib only — **not** a board emulation). The
shim directory is on the include path *only* for these tests and must never be
added to the firmware build.

`<ArduinoJson.h>` is the one exception to "thin": `shims/ArduinoJson.h`
includes the real library, vendored unmodified as the single-header release
in `vendor/ArduinoJson-v7.4.3.h` (the version all three firmwares pin in
`lib_deps`; bump both together). It is vendored rather than fetched so the
loop above needs no network and cannot test against a different version than
the devices run.

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
