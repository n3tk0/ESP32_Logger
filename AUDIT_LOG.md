# ESP32 Water Logger — Security & Architecture Audit Log

Persistent record of findings across all audit phases. Severity scale: **C**ritical / **H**igh / **M**edium / **L**ow / **I**nfo.

---

## Phase 1 — setup() / Hardware Init / LittleFS / RTC

Files: `ESP_Logger.ino`, `src/managers/HardwareManager.*`, `src/managers/StorageManager.*`, `src/managers/RtcManager.*`, `src/managers/ConfigManager.*`, `src/core/Globals.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 1.1 | H | ESP_Logger.ino:468-476 — `digitalRead()` over GPIO 0-10/18-21 before `pinMode()` runs (initHardware at L526). earlyGPIO_bitmask undefined on cold boot. | Move pinMode for wake pins above the earlyGPIO loop, or accept floating-input semantics in `getWakeupReason` (already has fallback). | Pending |
| 1.2 | H | ESP_Logger.ino:472-473 — Snapshot includes GPIO18-19 (USB D+/D- on C3 SuperMini). Plugged USB skews bitmask. | Skip 18-19 when `CONFIG_IDF_TARGET_ESP32C3 && USB CDC on Boot`. | Pending |
| 1.3 | H | HardwareManager.cpp:72 — `pinMode(pinFlowSensor, INPUT)` with no pull. YF-S201 needs INPUT_PULLUP. | `pinMode(config.hardware.pinFlowSensor, INPUT_PULLUP);` | Pending |
| 1.4 | M | HardwareManager.cpp:56-60 — `isPinSafe` lambda declared but never invoked in initHardware(). | Validate each `config.hardware.pin*` against isPinSafe; clamp to DefaultPins on failure. | Pending |
| 1.5 | C | HardwareManager.cpp:8-22 — `onFFButton`/`onPFButton` ISRs and `ffPressed`/`pfPressed` flags exist but never attached anywhere. Dead code in IRAM. | Either register via attachInterrupt for wake pins or delete the ISR + flag declarations. | Pending |
| 1.6 | C | ESP_Logger.ino:612-618 — In non-legacy mode mutexes depend on `TaskManager::init()`. On init failure, ApiHandlers calls `xSemaphoreTake(NULL)` → assert. | Move mutex creation BEFORE `TaskManager::init()` so they survive task-init failure; or refuse to start web server when init returns false. | Pending |
| 1.7 | C | StorageManager.cpp:12 + ConfigManager.cpp:361 — `LittleFS.begin(true, ...)` with `formatOnFail=true`. Transient corruption silently reformats partition. | Set formatOnFail=false; expose a `/factory_reset` confirmation path for format. | Pending |
| 1.8 | H | ConfigManager.cpp:529-558 — `saveConfig()` proceeds to write even when fsMutex acquire times out. | Return false on mutex timeout; do NOT write. Or use longer timeout. | Pending |
| 1.9 | H | ConfigManager.cpp:563 — `moduleRegistry.saveAll(LittleFS)` called AFTER releasing fsMutex; saveAll never takes it. | Move saveAll BEFORE `xSemaphoreGive(fsMutex)` or have saveAll acquire the mutex internally. | Pending |
| 1.10 | M | RtcManager.cpp:27-28 — `delete rtcWire` before `delete Rtc`. Rtc dtor may dereference dangling _wire reference. | Swap order: `delete Rtc; Rtc = nullptr; delete rtcWire; rtcWire = nullptr;` (delete BEFORE nulling, else leak). | Pending |
| 1.11 | M | RtcManager.cpp:71-77 — `backupBootCount` writes magic byte FIRST then bootcount bytes. Partial write leaves valid magic + bad value. | Write 4 bootcount bytes first, then magic last. Reverse order in backupBootCount. | Pending |
| 1.12 | L | RtcManager.cpp:46 — `RtcDateTime(__DATE__, __TIME__)` fallback regresses wall clock on every reflash. | Use 2024-01-01 00:00:00 baseline; surface "time invalid" in UI. | Pending |
| 1.13 | M | ESP_Logger.ino:540 — `_writeResetLog()` runs AFTER `bootCount++`. Reason for reset N logged under boot N+1. | Move `_writeResetLog()` BEFORE the `bootCount++` increment. | Pending |
| 1.14 | M | ESP_Logger.ino:543 — `OtaManager::boot()` runs after _writeResetLog. If FS op crashes, rollback never armed. | Move `OtaManager::boot()` to immediately after `Serial.begin`. | Pending |
| 1.15 | M | ESP_Logger.ino:329 — `_checkPinConflicts` reads entire platform_config.json without size cap. | Add `if (f.size() > 16*1024) return;` before `deserializeJson`. | Pending |
| 1.16 | M | ESP_Logger.ino:264-275 — `_detectPlatformMode` OOM silently downgrades to LEGACY. | Log error via Serial; ESP.restart() rather than mode regression on OOM. | Pending |
| 1.17 | L | ESP_Logger.ino:498-503 — Corrupt modules.json: `loadAll` returns false, `exists()` true → `saveAll` not called. File rot forever. | Quarantine: on parse fail rename to `.corrupt`, then call saveAll. | Pending |
| 1.18 | L | ESP_Logger.ino:485 — `isrDebounceUs` assignment is dead (button ISRs never attached). | Delete the line; remove `isrDebounceUs` global. | Pending |

---

## Phase 2 — FreeRTOS Tasks / ISRs / DataPipeline

Files: `src/tasks/*`, `src/pipeline/*`, `src/sensors/plugins/WaterFlowSensor.cpp`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 2.1 | C | TaskManager.cpp:32 + ESP_Logger.ino:454 — `bool` return of `init()` ignored. Caller proceeds with NULL queues. | `if (!TaskManager::init(*activeFS)) { shouldRestart = true; return; }` | Pending |
| 2.2 | H | TaskManager.cpp:33-155 — Partial-init failure paths leak created queues/mutexes/tasks; leaves `running=true`. | Add cleanup block: delete queues, give mutexes, delete tasks before each `return false`. | Pending |
| 2.3 | H | TaskManager.cpp:33 — `running = true` set BEFORE queues exist. Window of TaskManager::running=true / sensorQueue=NULL. | Move `running = true` to just before final `return true`. | Pending |
| 2.4 | H | TaskManager.cpp:130-150 init order — SensorTask + ProcessingTask created BEFORE StorageTask. storageQueue can fill before consumer exists. | Create StorageTask FIRST, then ExportTask, then producers (Sensor/SlowSensor/Process). | Pending |
| 2.5 | H | TaskManager.cpp:178 hard wait `pdMS_TO_TICKS(500)` shorter than SlowSensorTask's 500ms poll. Task can wake post-shutdown into freed memory. | Loop on `eTaskGetState() == eDeleted` per task, or wait 1.5× SlowSensorTask's poll. | Pending |
| 2.6 | C | WaterFlowSensor.cpp:41 — `gpio_isr_handler_add(pin, _isr, this)` with NO `gpio_isr_handler_remove` in dtor. `SensorManager::reloadConfig` deletes sensor → ISR fires on dangling `this`. | Add destructor: `gpio_isr_handler_remove((gpio_num_t)_pin);` | Pending |
| 2.7 | H | StorageTask.cpp:99,109,122 — `fsMutex` taken with `portMAX_DELAY`. Blocked SD pull → wedge for >30s, C4 watchdog resets. | Use `pdMS_TO_TICKS(3000)`; on timeout skip the write iteration and bump g_queueDrops. | Pending |
| 2.8 | H | ProcessingTask.cpp:48 — `xSemaphoreTake(webDataMutex, 0)` best-effort. Web reads cause silent ring-push drops. | Use `pdMS_TO_TICKS(5)`; or count drops in `g_ringDrops`. | Pending |
| 2.9 | H | ProcessingTask.cpp:67 — exportQueue send with timeout 0 → silent drops on WiFi backpressure. | Use `pdMS_TO_TICKS(10)`; the existing `g_queueDrops` counter is fine. | Pending |
| 2.10 | M | DataPipeline.cpp:13 — `volatile uint32_t g_queueDrops` incremented from 2+ tasks. RMW not atomic. | `std::atomic<uint32_t> g_queueDrops;` and `g_queueDrops.fetch_add(1, std::memory_order_relaxed);` | Pending |
| 2.11 | M | DataPipeline.h:47-72 RingBuffer marked SPSC but writer uses try-take webDataMutex; struct memcpy of 72B not torn-write safe on dual-core. | Either always serialize through webDataMutex, or add per-slot sequence counter. | Pending |
| 2.12 | M | DataPipeline.h:101-124 `collectMetricSeries` self-aliasing memmove pattern. Source/dest overlap when count > maxOut/2. | Use `memmove(out, out+maxOut-count, count*sizeof(float));` | Pending |
| 2.13 | M | HardwareManager.cpp:27 — `pulseCount++` in ISR is not atomic on RV32. | `__atomic_fetch_add(&pulseCount, 1, __ATOMIC_RELAXED);` | Pending |
| 2.14 | M | TaskManager.cpp:193-209 — `hb == 0` treated as "not started" indefinitely; deadlocked task at line 16 never tripped. | Seed each heartbeat slot in init() with `millis()`; remove the `hb==0` exemption. | Pending |
| 2.15 | M | SensorTask.cpp:24-28, SlowSensorTask:21-25, StorageTask:17-24 — three copies of `nowEpochSafe`. NTP jump at runtime corrupts LiveAggregator._lastFlushEpoch. | Centralize in `pipeline/TimeSource.cpp`. Detect epoch jumps > 86400 and rebase _lastFlushEpoch. | Pending |
| 2.16 | M | TaskManager.cpp:115-126 — Mirror mode holds fsMutex for primary + mirror appendRow. SD writes block 50-100ms. | Release fsMutex between primary and mirror, or use dedicated mirrorFsMutex. | Pending |
| 2.17 | L | LiveAggregator.cpp:198-201 — `_lastFlushEpoch = nowEpoch` on first call. NTP jump after that causes no rows until next jump anchor. | Guard with monotonic millis() in parallel; flush when EITHER threshold passes. | Pending |
| 2.18 | L | ExportTask.cpp:50 — Shutdown flush calls `exportManager.sendAll` (blocking TLS HTTP) inside task-exit path. ESP.restart can race. | Skip final flush on shutdown (`if (running && batchCount > 0)`), or cap to 1s. | Pending |
| 2.19 | M | AlertEngine.evaluate called from ProcessingTask.cpp:57 with no caller-side mutex. `handleApiAlertsSave` mutates rules concurrently. | AlertEngine must take its internal `_mutex` in evaluate() (verify in Phase 15). | Pending |

---

## Phase 3 — WebServer.cpp / ApiHandlers.cpp

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 3.1 | C | WebServer.cpp:1383 `/factory_reset` — no CSRF, no rate limit. LAN attacker can wipe device with one POST. | Add `if (rateLimit429(r)) return; if (csrfBlock(r)) return;` at top. | Pending |
| 3.2 | C | WebServer.cpp:1404 `/restart` — no CSRF (rate limited only). | Add `csrfBlock(r)` guard. | Pending |
| 3.3 | C | WebServer.cpp:1811 `/do_update` (OTA) — no CSRF, no rate limit. | Add both guards. | Pending |
| 3.4 | H | WebServer.cpp:1336 `/sync_time`, 1355 `/rtc_protect`, 1363 `/flush_logs`, 1368 `/backup_bootcount`, 1373 `/restore_bootcount` — no rate limit, no CSRF. | Add `rateLimit429` + `csrfBlock` to each. | Pending |
| 3.5 | H | WebServer.cpp:1750 `/wifi_scan_start`, 1757 `/wifi_scan_result` — no CSRF; mutates WiFi mode. | Add CSRF guard; protect from cross-origin DoS. | Pending |
| 3.6 | C | WebServer.cpp:1415 `/download` — does not call `isPathProtected`. `/download?file=/config.bin` leaks WiFi creds. | After sanitizePath: `if (isPathProtected(path)) { r->send(403,...); return; }` | Pending |
| 3.7 | C | WebServer.cpp:1022-1024 `/export_settings` — emits `apPassword`/`clientPassword` plaintext. | Replace passwords with `"***"` mask; require explicit opt-in param to include them. | Pending |
| 3.8 | C | ApiHandlers.cpp:236 — `new SensorReading[MAX_RAW]` without `std::nothrow`. bad_alloc → abort. | Use `new(std::nothrow)` and null-check. | Pending |
| 3.9 | H | ApiHandlers.cpp:73-130 — `raw` AND `agg` both allocated even when historicalPath sets aggCount=0. ~34 KB transient peak. | Allocate `agg` only after path decision; defer `raw` allocation when ring is empty. | Pending |
| 3.10 | M | ApiHandlers.cpp:53-58 — sensorFilter/metricFilter c_str() captured but inconsistent with the lifetime fix applied to agg/mode at line 106-112. | Apply the same copy-to-local-buffer pattern. | Pending |
| 3.11 | M | ApiHandlers.cpp:611-647 wifi-test — globals updated without atomic_thread_fence. Reader can see WT_SUCCESS with stale ip/rssi. | `std::atomic<WifiTestState> g_wtState;` with release/acquire. | Pending |
| 3.12 | H | WebServer.cpp:1653 `static String _importBuf` — shared across concurrent /import_settings uploads. Interleave corrupts JSON. Never reset on disconnect. | Move to `request->_tempObject`; clear on disconnect. | Pending |
| 3.13 | C | WebServer.cpp:1389 `/factory_reset` — `xSemaphoreTake(fsMutex, 2000)` return value discarded; unconditional `xSemaphoreGive` at 1395 asserts on un-held mutex. | Capture return; only give if take succeeded. | Pending |
| 3.14 | H | WebServer.cpp:1608 — Upload fsMutex held for entire body (potentially MB). | Take mutex per chunk write only; release between writes. | Pending |
| 3.15 | H | WebServer.cpp:1396 `safeWiFiShutdown()` runs from AsyncTCP worker with 300ms of delays + WIFI_OFF. Tears down own netif. | Schedule via `shouldRestart=true` flag; perform shutdown from loop(). | Pending |
| 3.16 | M | ApiHandlers.cpp:488-493 `/api/ota/rollback` — `delay(200)` on AsyncTCP worker. | Use deferred flag like NTP sync pattern. | Pending |
| 3.17 | M | WebServer.cpp:1289-1330 `/set_time` — ~120ms of `delay()` in RTC writes from AsyncTCP worker. | Move RTC mutation to loop() deferred handler. | Pending |
| 3.18 | M | WebServer.cpp:1422 `/download` TOCTOU between exists() and beginResponse(). | Null-check `resp` before `addHeader`. | Pending |
| 3.19 | H | ApiHandlers.cpp:312-321 `/api/config/platform` — holds configMutex but SensorTask iterates sensor table WITHOUT configMutex. Use-after-free on reload. | Wrap `sensorManager.tickFiltered` body in configMutex, OR signal quiesce flag. | Pending |
| 3.20 | M | ApiHandlers.cpp:807-873 `/api/backup` — takes configMutex but inhaleJsonFile reads files without fsMutex. | Acquire fsMutex around each `inhaleJsonFile` call. | Pending |
| 3.21 | L | `_doSleep` (ESP_Logger.ino:175-193) — does not call `safeWiFiShutdown`. Hybrid path at 1019 only conditional. | Always call safeWiFiShutdown in _doSleep if WiFi.getMode() != WIFI_OFF. | Pending |

---

## Phase 4 — Frontend ↔ Backend Contract (www/)

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 4.1 | C | iot-extensions.js:705-715 — `/api/sensors` returns `{sensors:[...]}` but populateHealthPage expects raw Array. Health page renders nothing. | `var sensors = (data && data.sensors) || []; if (!sensors.length) return; renderHealthGrid(sensors);` | Pending |
| 4.2 | M | sensors.js:104 — reads `d.last_sweep_ms` which backend never emits. | Emit `last_sweep_ms` from SensorManager::toJson; OR remove the UI label. | Pending |
| 4.3 | C | pages.js:812-822 `filesDelete` — sends GET to /delete (backend only registers POST). All Files-page deletions silently 405. | Add `{ method: "POST" }` to fetch options. | Pending |
| 4.4 | H | pages.js:812,877,901,865 — file ops send no CSRF token (matches backend gap from 3.4). | Add CSRF param + retry logic on 403. | Pending |
| 4.5 | C | settings.js:1525 `/do_update` — no CSRF. otaInit and otaUpload run without CSRF auth. | Append `csrf` to FormData; retry on 403. | Pending |
| 4.6 | H | settings.js:1665 `Modules.save` — JSON body, no `csrf` param. CsrfToken::require can't find param in JSON bodies. | Send `?csrf=<token>` in query string for JSON endpoints; or move CSRF check to header. | Pending |
| 4.7 | C | iot-extensions.js:603 POST /api/alerts — no CSRF token. Whole alert ruleset can be replaced unauthenticated. | Add `csrf` param; backend should also call `csrfBlock`. | Pending |
| 4.8 | H | All `fetch()` / `XMLHttpRequest` — no AbortController, no `xhr.timeout`. UI hangs forever on backend deadlocks. | Wrap fetches in `Promise.race(fetch, timeout(15000))`; set `xhr.timeout = 60000` for uploads. | Pending |
| 4.9 | M | settings.js:1404 otaUpload XHR — no timeout. Mid-flash crash hangs UI. | `xhr.timeout = 120000; xhr.ontimeout = function() { ... }` | Pending |
| 4.10 | M | pages.js:1000 `/api/live` polling — no in-flight guard. Backend stall queues many requests. | Track `liveInFlight`; skip tick if previous unfinished. | Pending |
| 4.11 | M | pages.js:963-980 EventSource — no client-side keepalive. Silent stall not detected. | Reset a 10s timer on each event; on timeout close+reopen. | Pending |
| 4.12 | M | core.js:438-451 CSRF cache — only `settingsSave` retries on 403. Other call sites do not. | Centralize: every mutating fetch uses a `postWithCsrf()` helper. | Pending |
| 4.13 | H | settings.js:1349 `_otaSha256` — SubtleCrypto unavailable on `http://` (Secure Context spec). AP mode always returns empty hash → server skips SHA-256 verification silently. | Detect missing SubtleCrypto; warn user; require manual hash entry; OR fall back to JS-side SHA implementation. | Pending |
| 4.14 | M | WebServer.cpp:1278 `/save_time` — writes timezone but never `dstOffsetHours`. UI never reads/writes it. DST stuck at 0. | Add `dstOffsetHours` param parse + UI input. | Pending |
| 4.15 | L | settings.js (network init) — `/export_settings` leaks plaintext passwords into hidden form fields. DevTools reveals. | Tied to 3.7; once masked, UI must request unmask explicitly. | Pending |
| 4.16 | L | pages.js:686 — filelist `truncated` flag from backend never read in UI. >500 files: silent partial list. | Render "showing first 500" banner when `d.truncated`. | Pending |

---

## Phase 5 — Build & Config Headers

Files: `arduino_build_flags.h`, `setup.h`, `core/Config.h`, `core/SensorTypes.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 5.1 | M | setup.h:121 `DEFAULT_FLOW_PIN 4` vs Config.h:62 `DefaultPins::FLOW_SENSOR = 21` — disagreeing defaults; setup.h macros never referenced in src/. | Delete `DEFAULT_SDA/SCL/FLOW_PIN` from setup.h or wire them through DefaultPins. | Pending |
| 5.2 | M | setup.h:140-143 `#define CONFIG_FREERTOS_UNICORE 1` — overrides sdkconfig. ABI mismatch with prebuilt IDF .a archive. | Convert to `#if` guard that errors if value disagrees with `__has_include(<sdkconfig.h>)` import. | Pending |
| 5.3 | C | setup.h:246-254 — default `WEB_BASIC_AUTH_USER "admin"` / `WEB_BASIC_AUTH_PASS "admin"` shipped when `WEB_BASIC_AUTH_ENABLED=1`. | `strcmp` cannot run in `#if` (preprocessor has no string compare). Two viable fixes: **(a)** require user to define a sentinel macro, e.g. `#if WEB_BASIC_AUTH_ENABLED && !defined(WEB_BASIC_AUTH_CREDS_OVERRIDDEN)` → `#error "Define WEB_BASIC_AUTH_CREDS_OVERRIDDEN after setting non-default creds"`; **(b)** in a `.cpp` TU compile-time check with C++17: `static_assert(std::string_view{WEB_BASIC_AUTH_USER} != "admin" \|\| std::string_view{WEB_BASIC_AUTH_PASS} != "admin", "Override default basic-auth credentials before enabling AUTH");`. Choose (b) if toolchain is C++17+; else (a). | Pending |
| 5.4 | L | setup.h:92-106 — all five cloud exporters default ENABLED. ~30-40 KB flash bloat unused at runtime. | Default OFF; enable per-deployment via -D flags. | Pending |
| 5.5 | I | setup.h:179-198 — no DRAM budget validation in comments. | Add `static_assert(STACK_SENSOR_TASK + STACK_PROCESS_TASK + ... < 50000, "tasks exceed DRAM budget");` | Pending |
| 5.6 | M | Config.h:13-15 — `VERSION 4.2.0` but ESP_Logger.ino:2 and project docs say v5.1.0. | Bump VERSION_MAJOR/MINOR to match release; update getVersionString. | Pending |
| 5.7 | C | Config.h:62 `FLOW_SENSOR = 21` — GPIO21 = USB D+ on ESP32-C3 SuperMini. Defaults break USB programming. | Switch default to GPIO10 (or another free pin); document per-board overrides. | Pending |
| 5.8 | C | Config.h:67-70 — SD pins 10-13. GPIO11-17 are SPI flash on C3 → SD storage broken out of the box. | Document "SD requires non-C3 target"; or change defaults to GPIO0/1/8/9 for compatibility; sanitize SD pins like wake pins. | Pending |
| 5.9 | C | Config.h:63-65 — RTC pins 5/6/7 collide with XIAO ESP32-C3 I2C bus (SDA=6, SCL=7). Cannot use DS1302 + I2C sensors simultaneously. | Either move RTC defaults to GPIO20-21 (USB pins, unused if USB CDC off) or document mutual exclusion. | Pending |
| 5.10 | M | Config.h:109 `#pragma pack(push, 1)` — unaligned float access on RV32 (e.g. `LoggerConfig::humidityCorrectionKappa` at odd offset). Performance penalty on hot path. | Remove pack; let compiler align; add `static_assert(sizeof(DeviceConfig) < 2048)`. Migration code already uses offsetof, so removing pack changes file format — bump CONFIG_VERSION to 14. | Pending |
| 5.11 | M | Config.h:173-189 — `uint8_t pin*` cannot express -1 "unused". Sensor plugin `cfg["pin"] | -1` casts to 0xFF. | Use `int8_t` for pin fields; -1 = unused; sanitize negatives in load path. | Pending |
| 5.12 | L | Config.h:232 `uint8_t _reserved_lang` — permanently burned byte under pack(1). | Reuse as a future feature flag; rename to `_flags` and document. | Pending |
| 5.13 | L | Config.h:198-205 LoggerConfig has reserved[16]; other appended structs do not. | Add `uint8_t reserved[8]` tail to each *Config struct. | Pending |
| 5.14 | L | Config.h:253 magic 32/512 budget undocumented; misleading since DS1302 RAM is 31 B. | Replace magic numbers with `RTC_SLOW_LOG_BUDGET` macro + comment. | Pending |
| 5.15 | M | Config.h:99-104 `enum LoggingState` — no explicit underlying type. ESP_Logger.ino:1024 relies on signed-int comparison. | `enum LoggingState : int8_t { ... };` | Pending |
| 5.16 | M | SensorTypes.h:29-31 — `SensorReading() { memset(this, 0, sizeof(*this)); }` is UB if any non-trivial member added. | `SensorReading() = default;` with in-class member initializers (`uint32_t timestamp = 0;` etc.). | Pending |
| 5.17 | H | SensorTypes.h:55-61 `toJsonLine()` emits unescaped `%s` for sensorId/sensorType/metric/unit. User-supplied sensorId via platform_config.json can inject JSON. | Implement `jsonEscape(out, dst, dstLen)` helper; escape every string field before snprintf. | Pending |
| 5.18 | L | SensorTypes.h:86-94 / 113-122 parseBucket/parseMode case-sensitive lowercase only. | Lowercase the input first; or document the requirement in the API spec. | Pending |
| 5.19 | I | SensorTypes.h:67-74 AggMode enum — values stored numerically in JSON config. Inserting AGG_MEDIAN between LTTB and SUM would silently reassign historical configs. | Add comment "APPEND-ONLY; never renumber". | Pending |

---

## Phase 6 — Core Globals & Module Framework

Files: `core/Globals.*`, `core/IModule.h`, `core/ModuleRegistry.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 6.1 | M | Globals.cpp:9 — `AsyncWebServer server(80)` static-init constructor; port hard-coded; static init order across TUs is undefined. | Wrap in `getServer()` accessor with function-local static; expose port via `setup.h` macro. | Pending |
| 6.2 | M | Globals.cpp:33-36 — RTC_DATA_ATTR initializers are applied by 2nd-stage bootloader only on cold boot. No magic-word guard on `logBufferCount`. | Add `RTC_DATA_ATTR uint32_t rtcMagic;` checked at boot; on mismatch, zero `logBuffer[]` and `logBufferCount`. | Pending |
| 6.3 | H | Globals.h:50-51 — `extern String wakeUpButtonStr` / `cycleStartedBy` mutated from loop() AND read by AsyncTCP worker (WebServer.cpp:117). String buffer pointer triple is not atomic → UAF read. | Replace with `char wakeUpButtonStr[16]` + `char cycleStartedBy[16]`; or guard with webDataMutex on every access. | Pending |
| 6.4 | I | Globals.h:74 `volatile uint32_t pulseCount` — restated from 2.13. Same fix. | See 2.13. | Pending |
| 6.5 | L | Globals.h:106 `PlatformMode g_platformMode` non-volatile/non-atomic. Single-byte enum, but C++ memory model requires atomic for cross-task visibility. | `std::atomic<uint8_t> g_platformMode;` reads/writes via .load()/.store(memory_order_acquire/release). | Pending |
| 6.6 | I | Globals.cpp:75 `isrDebounceUs` — dead variable (button ISRs unused). Restated from 1.18. | Delete. | Pending |
| 6.7 | I | Globals.h — 130+ extern globals form the architectural antipattern driving multi-task races. | Long-term: refactor to subsystem-owner pattern (each manager exposes accessor functions). | Pending |
| 6.8 | M | IModule.h:42-43 — Asymmetric error reporting: `load()` returns bool, `save()` returns void. JSON capacity overflow silently truncates. | `virtual bool save(JsonObject cfg) const = 0;` and propagate to ModuleRegistry::saveAll. | Pending |
| 6.9 | L | IModule.h:54 — `tick(uint32_t nowMs)` never called. Dead virtual misleads module authors. | Either wire a `tickAll()` from main loop / a TickerTask, or remove the method. | Pending |
| 6.10 | L | IModule.h:75 — schema() return is unvalidated; malformed JSON silently breaks the UI (settings.js:1702). | Add `bool validateSchema(const char* s)` helper called at registration time in `ModuleRegistry::add`. | Pending |
| 6.11 | C | ModuleRegistry.cpp:130-138 `startAll()` exists but is NEVER CALLED at boot. Modules that perform setup in start() silently don't initialize. | Add `moduleRegistry.startAll();` to ESP_Logger.ino:setup() after `loadAll`. | Pending |
| 6.12 | H | ModuleRegistry.cpp:78 — `_modules[i]->load(slice)` return value DISCARDED. Validation failures silently swallowed; module ends up enabled with garbage. | `bool ok = _modules[i]->load(slice); if (!ok) { Serial.printf("[ModuleRegistry] %s load() failed\n", id); _modules[i]->setEnabled(false); }` | Pending |
| 6.13 | M | ModuleRegistry.cpp:63-67 — Parse error leaves corrupt modules.json untouched; subsequent boots load defaults silently forever. | Quarantine: `fs.rename(path, "/config/modules.json.corrupt"); saveAll(fs, path);` | Pending |
| 6.14 | H | ModuleRegistry.cpp:110 — `serializeJson` short-write check only catches `n == 0`. Truncated-but-nonzero output passes through rename. | Compare against `measureJson(doc)`; abort + remove tmp if shorter. | Pending |
| 6.15 | M | ModuleRegistry.cpp:36-44 + 84-127 — saveAll does not take fsMutex; loadAll's crash-recovery rename also unlocked. | Acquire `fsMutex` at function entry of saveAll; expose `MutexGuard` helper for symmetry. | Pending |
| 6.16 | L | ModuleRegistry.cpp:116-120 comment promises atomic rename on LittleFS but the class accepts any `fs::FS&`. SD/FAT rename-over-existing fails. | Add probe: if `fs.exists(path)` → `fs.remove(path)` first when target FS != LittleFS. | Pending |
| 6.17 | M | ModuleRegistry.cpp:55-60 — Oversize-file rejection returns false but doesn't quarantine. File stays oversize across reboots. | Same fix as 6.13. | Pending |

---

## Phase 7 — Web Auth & Utilities

Files: `utils/Utils.*`, `web/CsrfToken.*`, `web/RateLimiter.*`, `web/WebServer.h`, `web/ApiHandlers.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 7.1 | H | Utils.cpp:69-76 `isPathProtected` — only covers `/config.bin`, `/bootcount.bin`, `/reset_log.txt`, `/_setup`. Misses `/alerts.json`, `/platform_config.json`, `/config/modules.json`, `/config/sensors.json`, `/config.tmp`. All deletable + downloadable. | Extend list; add prefix check for `/config/` directory; protect `/platform_config.json`; remove `/config.tmp` if found stale. | Pending |
| 7.2 | L | Utils.cpp:30 — control-char filter only checks `< 0x20` and `0x7f`. Bytes 0x80-0xFF (UTF-8 lead bytes, non-ASCII) pass. Not a security issue but allows weird filenames. | Document as design choice or restrict to ASCII printable. | Pending |
| 7.3 | L | Utils.cpp:57 `sanitizeFilename` 64-char cap. Generated datalog filenames (prefix + device id + timestamp + .txt) can approach 50-60 chars; close margin. | Bump to 96 chars; verify all generator paths against new limit. | Pending |
| 7.4 | M | Utils.cpp:83-134 `deleteRecursive` uses heap `std::vector` work-stack with no size cap. A crafted/corrupt FS tree with many directories can OOM the AsyncTCP worker. | Add `if (stack.size() > 256) return false;` to abort deep recursion. | Pending |
| 7.5 | M | CsrfToken.cpp:7-21 `ensureToken()` — two concurrent first `/api/csrf-token` requests can both pass `if (s_token[0]) return;` and interleave the 16 esp_random() writes. Single AsyncTCP worker mitigates today, but the function is callable from `require()` on any mutating handler. | Add a guard byte set BEFORE the loop: `static volatile bool initialised = false; if (initialised) return;` then write s_token, then `initialised = true;` with proper barrier. | Pending |
| 7.6 | M | CsrfToken — Token never expires within a boot session. A plaintext-HTTP capture remains valid until reboot. Combined with no HTTPS option, sniffed token = full mutating-API access. | Rotate token after N hours OR after every successful mutating request (one-shot tokens). | Pending |
| 7.7 | M | CsrfToken.h:33-35 — `csrfBlock(req)` returns inverse of `require()`. Reads "blocked when require failed" but semantic naming is backwards (`require` true = OK; `csrfBlock` true = failure). High maintenance footgun. | Rename to `csrfFailed(req)` or change `require()` semantics to return true=blocked for consistency. | Pending |
| 7.8 | H | CsrfToken.cpp:52-56 — looks for `csrf` only in form/query params, never headers. JSON-body endpoints (POST /api/modules/:id, POST /api/alerts) have no parsed params → check always fails. Mirrors Phase 4 #4.6 root cause. | Accept `X-CSRF-Token` header (ESPAsyncWebServer headers exposed via `req->getHeader`); OR require token via query string on JSON endpoints. | Pending |
| 7.9 | L | CsrfToken — GET `/api/csrf-token` is unauthenticated (WebServer.cpp:678). Anyone on AP can fetch token. By design per CsrfToken.h:14-20 double-submit pattern; flagged because it pairs poorly with plaintext HTTP. | Document the threat model; recommend WEB_BASIC_AUTH_ENABLED + HTTPS reverse-proxy for internet exposure. | Pending |
| 7.10 | L | RateLimiter.h:21-32 `allow()` — read-modify-write on `_tokens`/`_lastRefill` not thread-safe. Single AsyncTCP worker context masks the race today. | `std::atomic<uint32_t>` with `compare_exchange_weak` loop. | Pending |
| 7.11 | M | RateLimiter — single global bucket. One client can starve every other client. No per-IP tracking. | For LAN single-user devices this is acceptable; document. If concern: add a 4-entry LRU per-IP bucket map. | Pending |
| 7.12 | L | RateLimiter.cpp:6-7 — `_tokens = MAX_TOKENS` at static-init, `_lastRefill = 0`. First call from boot (millis()=N) computes elapsed=N, add=N/200, clamped to 20. First 20 requests within first second always succeed regardless of true rate. | Initialise `_lastRefill = millis()` at start of `allow()` if zero. | Pending |
| 7.13 | I | RateLimiter only applies to mutating handlers (each calls `rateLimit429` explicitly). Read endpoints (`/api/status`, `/api/live`, `/api/data`) are unbounded. | AsyncTCP worker is the implicit limiter; acceptable. Consider per-endpoint caps on heavy reads (`/api/data` with limit=300 allocates ~17 KB). | Pending |
| 7.14 | I | WebServer.h / ApiHandlers.h — Trivial header declarations; documentation lists Bulgarian descriptions. | [MODULE SAFE] | N/A |

---

## Phase 8 — HW / Storage / RTC / WiFi Manager Headers

Files: `managers/HardwareManager.*`, `managers/StorageManager.h` (+ cpp re-pass), `managers/RtcManager.*`, `managers/WiFiManager.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 8.1 | H | HardwareManager.cpp:36-48 `debounceButton` — `lastFFDebounceTime`/`lastPFDebounceTime` initialised to 0 in Globals.cpp:60-61. First call from loop() sees `millis() - 0 > debounceMs` always TRUE. If pin reads ACTIVE on power-on (button held during boot), counter immediately latches → spurious cycle start. | Initialise `lastFFDebounceTime = lastPFDebounceTime = millis()` in initHardware() AFTER pinMode. Also seed `last*ButtonState` from current digitalRead. | Pending |
| 8.2 | H | StorageManager.cpp:91-123 `generateDatalogFileOptions` — builds raw `<option>` HTML by concatenating `fullPath` with no HTML-escaping. Filename containing `"`, `<`, `>`, `&`, or `'` breaks markup or injects script. **Dead code today** (no callers — verified via grep) but linked into firmware and still re-enabled by legacy paths. | Delete `generateDatalogFileOptions` + `countDatalogFiles` from `.cpp/.h` (also dead). Or escape via `htmlEscape()` helper. | Pending |
| 8.3 | M | StorageManager.cpp:91-152 — `std::vector<String> dirs` recursion stack uncapped. Same OOM risk as Utils.cpp `deleteRecursive` (7.4). | Cap recursion depth via `if (dirs.size() > 256) break;`. | Pending |
| 8.4 | M | StorageManager.cpp:85-89 `getStorageBarColor` — declared/defined, **no callers** (grep clean). Dead. | Delete from `.cpp/.h`. | Pending |
| 8.5 | H | RtcManager.cpp:36-67 `initRtc` — calls `Rtc->SetIsWriteProtected(false)` at line 36 but **never re-enables write protection** at the end of init. RTC stays writable forever; any code path with `Rtc->SetDateTime` is unguarded against accidental writes (compare to WebServer.cpp:1316 RAII guard in `/set_time`). | After RTC init success, `Rtc->SetIsWriteProtected(true);`. WiFiManager.cpp:179-186 and `/set_time` already wrap their writes in unprotect/write/protect cycles. | Pending |
| 8.6 | H | RtcManager.cpp:78-80 `backupBootCount` — opens `/bootcount.bin` in `"w"` (truncate) mode and writes raw 4 bytes. NO atomic write (no `.tmp` + rename). Power loss mid-write = empty or 1-3 byte file. Next `restoreBootCount` reads truncated bytes → corrupt counter. | Mirror saveConfig pattern: write to `/bootcount.tmp`, fsync via close, `LittleFS.rename(tmp, BOOTCOUNT_BACKUP_FILE)`. | Pending |
| 8.7 | M | RtcManager.cpp:93-96 `restoreBootCount` — discards `f.read()` return value. On short file (truncated by 8.6 power-loss scenario), partial overwrite of `bootCount`; remaining bytes keep old stack/BSS state. | `size_t n = f.read(...); if (n != sizeof(bootCount)) bootCount = 0;` | Pending |
| 8.8 | M | RtcManager.cpp:71-80 `backupBootCount` — does not acquire `fsMutex` despite writing LittleFS. Race vs `saveConfig` (already taking it) and `StorageTask.appendRow`. | `if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(2000)) == pdTRUE) { ...; xSemaphoreGive(fsMutex); }`. | Pending |
| 8.9 | L | RtcManager.cpp:212-215 — ESP32/S2/S3 EXT1 wake path silently `return` when wakeupMode != ACTIVE_HIGH. User configures ACTIVE_LOW → device never wakes from GPIO. No status reported. | Set `statusMessage = "EXT1 wake requires ACTIVE_HIGH on this chip"` so the UI surfaces it. | Pending |
| 8.10 | M | RtcManager.cpp:53-56 — 3-iter retry loop calls `delay(10) + delay(10) + delay(100) = 120ms` per iteration × 3 = up to 360ms during `initRtc`. Runs BEFORE OtaManager::boot (ESP_Logger.ino:543). If a pending-verify image hangs here, rollback watchdog never arms. Tied to 1.14. | Move OtaManager::boot earlier (before initHardware/initRtc) per 1.14, OR add a heartbeat write inside the retry loop. | Pending |
| 8.11 | I | HardwareManager.cpp:26 — `ISR_DEBOUNCE_MICROS=1000` caps pulse rate at 1000 Hz ≈ 133 L/min for YF-S201 (450 pulses/L). Adequate for residential but undocumented. | Add `// Caps max measurable flow at ~133 L/min for YF-S201, ~100 L/min for YF-S403` comment near ISR. | Pending |
| 8.12 | I | RtcManager.cpp:30-33 — `new ThreeWire(...)` and `new RtcDS1302(...)` without `std::nothrow`. Bad_alloc on heap-pressured boot aborts. | `new(std::nothrow)`; on failure set `rtcValid=false; return;`. | Pending |
| 8.13 | I | HardwareManager.h / StorageManager.h / RtcManager.h / WiFiManager.h — trivial declarations; risk surface is in the corresponding .cpp files. | [MODULE SAFE for headers] | N/A |

---

## Phase 9 — Config / OTA / DataLog Managers

Files: `managers/ConfigManager.*`, `managers/OtaManager.*`, `managers/DataLogger.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 9.1 | M | ConfigManager.cpp:40-46 — `if (config.datalog.maxEntries == 0)` resets FIVE unrelated fields (maxEntries, includeBootCount, includeExtraPresses, postCorrectionEnabled, timestampFilename). User cannot validly set maxEntries=0 to disable rotation without losing other settings. | Split into per-field zero checks; or use a dedicated `if (!datalog_initialised)` magic flag. | Pending |
| 9.2 | M | ConfigManager.cpp:119, 137 — `isRtcWakePinC3` and `isSafePinC3` hardcoded for ESP32-C3 pin ranges (≤5 for wake, !=11..17 for safe). ESP32-S3/S2 builds get false-invalid verdicts and reset to wrong defaults. | Gate via `#if CONFIG_IDF_TARGET_ESP32C3 ... #elif CONFIG_IDF_TARGET_ESP32S3 ...` with per-target pin tables. | Pending |
| 9.3 | H | ConfigManager.cpp:441-449 — Migration path copies header preamble (`magic`..`resetBootCountAction`) byte-for-byte via `memcpy(&config, rawBuf, headerSize)` with NO per-field version guard. If a pre-v6 binary had different layout (no `_reserved_lang` byte, different field order), every subsequent byte is shifted. SAFE_COPY's partial-copy branch can also leave strings non-NUL-terminated. No tests for cross-version migration. | Add per-version offset tables; for unrecognised pre-v6 layout, refuse to migrate and reset to defaults. Verify via fuzz tests of older config blobs. | Pending |
| 9.4 | M | ConfigManager.cpp:194 `regenerateDeviceId` — bypasses fsMutex (same as 1.8), no CSRF on the API surface (4.x), and produces a deviceId change without reboot → external API consumers cache stale id. | Acquire fsMutex; require explicit user-initiated regen flow that warns about stale caches; bump a `deviceIdGeneration` counter clients can watch. | Pending |
| 9.5 | L | ConfigManager.cpp:352 `migrateConfig` ends with `saveConfig()` during setup() before fsMutex exists. Today benign (single-threaded). If migrate is ever called outside setup, race. | Add comment "migrate must run pre-task-init". | Pending |
| 9.6 | I | ConfigManager.cpp:21-113 `applyDefaults` long sequential `if !strlen()/badFloat()` chain. Maintainability concern only. | Split into per-section helper functions. | Pending |
| 9.7 | M | DataLogger.cpp:12-21 `countFileLines` — reads entire log byte-by-byte BEFORE every flush (called from line 69). O(file-size) per flush. On a 1 MB log, every cycle re-reads 1 MB. | Cache line count in RTC RAM or compute incrementally; only re-scan after rotation. | Pending |
| 9.8 | H | DataLogger.cpp:50-51 `trimLogFile` — `fs->remove(path); fs->rename(tmpPath, path);` is non-atomic. Power loss in the gap leaves NEITHER file present, losing the entire datalog history. Contrast with ConfigManager/ModuleRegistry which use rename-over-existing. | LittleFS rename overwrites atomically — drop the `remove` and just `rename(tmpPath, path)`. | Pending |
| 9.9 | M | DataLogger.cpp:73 `flushLogBufferToFS` — opens FILE_APPEND without fsMutex. Called from loop() AND from `/flush_logs` (AsyncTCP worker, WebServer.cpp:1364). Race vs StorageTask.appendRow, saveConfig, RtcManager.backupBootCount (8.8). | Acquire fsMutex (timeout 2000ms); return false on timeout so caller can retry. | Pending |
| 9.10 | L | DataLogger.cpp:60-65 — `folder` directory created without isPathProtected check. Malicious or careless config can target `/_setup` etc. | Validate against isPathProtected before mkdir; reject save_datalog with 400 if invalid. | Pending |
| 9.11 | L | DataLogger.cpp:174-180 — When `logBufferCount >= LOG_BATCH_SIZE` and flush failed, silently shifts array dropping oldest entry. No counter, no log. RTC slow memory backup loses data unobserved. | Increment a `g_logDrops` counter; expose via `/api/diag`. | Pending |
| 9.12 | M | OtaManager.cpp:25-34 `_logOtaEvent` — writes `/reset_log.txt` without fsMutex. Races against StorageTask, saveConfig, datalog flush. | Same fix family as 9.9 / 8.8 — acquire fsMutex. | Pending |
| 9.13 | L | OtaManager.cpp:114-128 `confirm()` — failure is permanent. Subsequent `tick()` calls keep retrying `esp_ota_mark_app_valid_cancel_rollback` every loop iteration, wasting CPU. | After first confirm failure, set `s_confirmFailed = true` and skip retries; log once. | Pending |
| 9.14 | L | OtaManager.cpp:132-148 `rollback()` — `esp_ota_mark_app_invalid_rollback_and_reboot` does NOT verify target partition has a valid image. If both slots corrupt, device bricks on reboot. Standard ESP-IDF behavior; out of scope of this firmware but worth documenting. | Pre-validate target via `esp_ota_get_state_partition(prev, &state)` before invoking rollback. | Pending |
| 9.15 | I | ConfigManager.h / OtaManager.h / DataLogger.h — trivial declarations. | [MODULE SAFE for headers] | N/A |

---

## Phase 10 — FreeRTOS Sensor Tasks

Files: `tasks/TaskManager.h`, `tasks/SensorTask.*`, `tasks/SlowSensorTask.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 10.1 | M | SensorTask.cpp:13 — `uint32_t pollMs = sensorManager.minReadIntervalMs();` fetched ONCE at task start. `/api/config/platform` reload mutates sensor intervals but SensorTask's local `pollMs` stays stale forever → reads lag (or run too fast) until reboot. | Re-read `minReadIntervalMs()` inside the loop, OR signal task via task-notification when reload happens. | Pending |
| 10.2 | M | SensorTask.cpp:13, 32 — If `sensorManager.count()==0` or `minReadIntervalMs()` returns 0, `vTaskDelay(pdMS_TO_TICKS(0))` becomes a yield-only busy-loop. Empty sensor config → 100% CPU on SensorTask + cache-thrashes the C4 watchdog clock. | Clamp: `if (pollMs < 50) pollMs = 50;` after the call; verify SensorManager guarantees a floor. | Pending |
| 10.3 | L | SensorTask.cpp:28, SlowSensorTask.cpp:25 — fallback `ts = millis()/1000` returns 0 for the first second of uptime. SensorTypes.h:21 reserves `ts=0` as "unknown" and LiveAggregator uses `_lastFlushEpoch==0` as "first call" sentinel. Collision during first 1 s after boot. | Bump: `ts = (millis()/1000UL) + 1;` to avoid 0 in the fallback path. | Pending |
| 10.4 | L | SlowSensorTask.cpp:31 — Hardcoded 500 ms outer poll. Not configurable, not data-driven. For SDS011 with 60-90 s internal cycles, 180 wakeups per useful read. Minor energy waste. | Compute from slow-sensor min interval, or expose a setup.h macro `SLOW_SENSOR_TICK_MS`. | Pending |
| 10.5 | L | SlowSensorTask.cpp:28 — Blocking sensor read (1.5-3 s for SDS011/PMS5003) runs WITHOUT refreshing `g_taskHeartbeat[TASK_IDX_SLOW_SENSOR]`. Current MAX_SILENCE_MS=30 s tolerates it; any future tightening would false-trip C4 watchdog. | Refresh heartbeat halfway through blocking reads via callback from the plugin, OR raise MAX_SILENCE_MS comment-doc. | Pending |
| 10.6 | M | TaskManager.h:30-34 — `static TaskHandle_t hSensor/hSlowSensor/hProcess/hStorage/hExport` declared PUBLIC. Any TU can write `TaskManager::hSensor = nullptr` and silently break `checkHealth`/diagnostics. | Make private; expose via `static TaskHandle_t getHandle(TaskIndex)` accessor. | Pending |
| 10.7 | M | TaskManager.h:36 — `static volatile bool running;` — volatile gives no-cache but no cross-task memory ordering. Tasks may observe stale `running==true` for several cycles after main sets false → late shutdown. | `static std::atomic<bool> running;` with `running.store(false, std::memory_order_release)` on shutdown. | Pending |
| 10.8 | M | TaskManager.h:25 / TaskManager.cpp:193 — `checkHealth()` inspects heartbeat staleness only. A crashed task's heartbeat byte holds whatever it last wrote; 30 s pass before watchdog fires. | Add fast path: `if (handle && eTaskGetState(handle) == eDeleted) return false;` before heartbeat check. | Pending |
| 10.9 | I | SensorTask.cpp:30 — passes `sensorQueue` directly to `sensorManager.tickFiltered`; manager iterates sensor table without configMutex (already 3.19 / will revisit Phase 15). | See 3.19. | Pending |
| 10.10 | I | TaskManager.h / SensorTask.h / SlowSensorTask.h — header surface is small; principal risk in `.cpp` (already covered Phase 2 + this phase). | [MODULE SAFE for headers] | N/A |

---

## Phase 11 — FreeRTOS Pipeline Tasks

Files: `tasks/ProcessingTask.*`, `tasks/StorageTask.*`, `tasks/ExportTask.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 11.1 | M | ProcessingTask.cpp:14-26 — `isPlausible()` only validates temperature/humidity/pressure/pm25/pm10/tvoc/eco2/flow_rate/wind_speed. Missing entirely: voltage, current, uv_index, lux/light, distance, soil_moisture, AND `co2` (SCD4x emits `co2`, not `eco2`). Catch-all `return true` at L25 means these never get QUALITY_ERROR flagged. | Add per-metric bounds for every metric a registered plugin emits; reject by default (`return false`) for unknown metrics; log unknown-metric exactly once. | Pending |
| 11.2 | L | ProcessingTask.cpp:17 — Pressure bounds 500..1200 assume `hPa`. A plugin emitting Pa (101325) is silently rejected. SensorReading has no unit-aware validator. | Either canonicalise units at plugin level OR include unit in the plausibility key. | Pending |
| 11.3 | M | ProcessingTask.cpp:42-51 — `webRingBuf.push(r)` runs even when `r.quality == QUALITY_ERROR`. UI dashboard displays error values mixed with good data. | Guard: `if (r.quality != QUALITY_ERROR && webDataMutex && xSemaphoreTake(...)) { push; give; }`. | Pending |
| 11.4 | M | ProcessingTask.cpp:57 — `alertEngine.evaluate(r, r.timestamp)` passes timestamp which can be 0 (SensorTask fallback, see 10.3). AlertEngine.Rule.condFirstMetTs=0 collides with that → first trigger at ts=0 makes second reading at ts=1 elapse "1 s" → false short-duration trigger. | Skip evaluate when `r.timestamp < 1000000000` (no real wall-clock); also ensure SensorTask never emits ts=0 (tied to 10.3 +1 fix). | Pending |
| 11.5 | M | StorageTask.cpp:32-33 — `StorageTaskParam cfg = p ? *p : StorageTaskParam{};` copies params at task start. `/api/config/platform` reload of logger.* fields doesn't propagate (csvLoggingEnabled, aggregationIntervalSec, humidityCorrection, kappa). Same staleness class as 10.1. | Re-read from `*p` once per outer loop iteration, or signal task via task-notification on reload. | Pending |
| 11.6 | H | StorageTask.cpp:91-95 — Inner drain loop `while (xQueueReceive(...100ms) == pdTRUE)` can run for many seconds under sustained sensor burst. Each iteration blocks up to 100 ms; with continuous input the loop never falls through. Outer-loop heartbeat at L84 thus never refreshes → C4 watchdog (30 s) fires under legitimate high-throughput conditions. | Cap inner drain: `int drained = 0; while (... && drained++ < 32) ...;` then fall through every outer iteration. Or refresh heartbeat inside the inner loop. | Pending |
| 11.7 | L | StorageTask.cpp:91 — `feedEpoch = nowEpochSafe()` computed ONCE before the drain loop. All items in a multi-second burst share the same timestamp regardless of arrival order. FlowRunLogger duration accounting blurred. | Compute per-item inside the loop, or use the SensorReading's own `r.timestamp` where valid. | Pending |
| 11.8 | H | ExportTask.cpp:31, 42 — `exportManager.sendAll(batch, batchCount)` blocks on TLS/HTTP/MQTT. With 5 enabled exporters × ~30 s socket timeout = up to 150 s blocked. ExportTask heartbeat at L22 only refreshes between iterations → C4 watchdog (30 s) false-positive restart during WiFi outages. | Refresh heartbeat between exporters inside sendAll (callback hook); or use a per-exporter shorter timeout (5 s). | Pending |
| 11.9 | L | ExportTask.cpp:38-45 — Two flush paths (full-batch at L29-34 + batchFull/timeout at L38-44) are correct but slightly redundant. Maintenance footgun. | Consolidate into one decision-point after L35. | Pending |
| 11.10 | I | ProcessingTask.h / StorageTask.h / ExportTask.h — trivial declarations or POD struct (StorageTaskParam). | [MODULE SAFE for headers] | N/A |

---

## Phase 12 — Pipeline Core

Files: `pipeline/DataPipeline.h`, `pipeline/AggregationEngine.*`, `pipeline/LiveAggregator.*`, `pipeline/FlowRunLogger.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 12.1 | H | AggregationEngine.cpp:49 — `size_t avgLen = nextBucketEnd - nextBucketStart;` underflows to huge size_t if `nextBucketEnd < nextBucketStart` after the clamp at L45 (`if (nextBucketEnd >= inLen) nextBucketEnd = inLen - 1`). When `nextBucketStart > inLen-1`, the subtraction wraps → loop at L50 iterates billions of times → device freezes. Triggerable via `/api/data?limit=N` with N close to inLen. | Add guard: `if (nextBucketEnd < nextBucketStart) continue;` BEFORE L49. Same guard for L67's `for (j = rangeStart; j < rangeEnd; ...)` loop. | Pending |
| 12.2 | H | AggregationEngine.cpp:67 — `for (size_t j = rangeStart; j < rangeEnd; j++)` — `rangeStart = i*bucketSize + 1`, `rangeEnd = nextBucketStart`. On the last LTTB iteration these can invert, causing same underflow as 12.1. | Cap iteration: `for (size_t j = rangeStart; j < rangeEnd && j < inLen; j++)`. | Pending |
| 12.3 | M | AggregationEngine.cpp:37 — `bucketSize = (double)(inLen - 2) / (double)(maxPoints - 2);` — divide-by-zero when `maxPoints == 2`. Subsequent cast `(size_t)(INF * x)` is UB. Earlier guard catches maxPoints==1 only. | Add `if (maxPoints <= 2) { out[0]=in[0]; if (maxPoints==2) out[1]=in[inLen-1]; return maxPoints; }`. | Pending |
| 12.4 | M | AggregationEngine.cpp:210 — `tmpBuf = new SensorReading[tmpSz];` without `std::nothrow`. Heap-pressured `/api/data` request → bad_alloc → abort. Inconsistent with line 213 fallback assumption. | `tmpBuf = new(std::nothrow) SensorReading[tmpSz];` (the fallback path already handles nullptr). | Pending |
| 12.5 | L | AggregationEngine.cpp:213 — On LTTB tmpBuf alloc failure, silently degrades to AGG_AVG. Caller has no signal. | Set a flag or return a sentinel count; surface via response header `X-Agg-Degraded: 1`. | Pending |
| 12.6 | M | LiveAggregator.cpp:122-125 — `if (strcmp(r.metric, "humidity") == 0) _lastHumidity = r.value;` ignores sensorType. Multi-zone deployment: SCD4x humidity in zone A overwrites BME280 humidity that was meant to correct SDS011 PM in zone B. Wrong kappa correction. | Match (sensorType, sensorId) tuple OR use BME-family only; document the policy. | Pending |
| 12.7 | M | LiveAggregator.cpp:202 — `if (nowEpoch < _lastFlushEpoch + _intervalSec) return false;` — If NTP correction moves epoch BACKWARD (e.g. local RTC was 1 day ahead, NTP rebases), no flush will EVER trigger again until reboot. | Use monotonic millis()-based parallel clock; flush when either threshold passes. Also detect epoch backwards jump > 60s and reset `_lastFlushEpoch = nowEpoch`. | Pending |
| 12.8 | L | LiveAggregator.cpp:27-34 — `xSemaphoreCreateMutex()` failure leaves `_mutex == nullptr`; the `Lock` helper at .cpp:19 treats null as "OK", silently running unsynchronised. Multi-task safety lost without indication. | Refuse to construct or set a hard-error flag; expose `isHealthy()`. | Pending |
| 12.9 | M | FlowRunLogger.cpp:130-131 — `volume = _volumeLatest - _volumeStart;` — when no volume reading was received before run start, `_volumeStart=0` (per L98 default), but `_volumeLatest` may hold a cumulative counter accumulated since boot → first run reports a wildly inflated volume. | Track a "has-seen-volume-since-run-start" bool; if false at close, mark volume column as empty rather than computing a bogus delta. | Pending |
| 12.10 | M | FlowRunLogger.cpp:156-179 — File write opens/appends/closes WITHOUT fsMutex. Race vs StorageTask.appendRow, ConfigManager.saveConfig, DataLogger.flush. Same fsMutex-bypass family as 8.8, 9.9, 9.12. | Acquire fsMutex (with timeout) around the open/write/close block. | Pending |
| 12.11 | M | FlowRunLogger.cpp:60-66 `_enforceSizeRotation` — `_fs->remove(bak); _fs->rename(path, bak);` non-atomic; power loss in the gap deletes the original. Same class as 9.8. | LittleFS rename overwrites atomically — drop the explicit remove. | Pending |
| 12.12 | H | DataPipeline.h:47-56 RingBuffer::push — ordering: writes `_buf[h%N] = r` (L49), advances `_tail` (L53) with relaxed memory order BEFORE the data write completes, THEN publishes `_head` with release (L55). A concurrent reader observing the new tail (relaxed load) while the writer is mid-`memcpy` of the slot can read a torn 72-byte SensorReading. Risk grows on dual-core targets (S3); single-core C3 + FreeRTOS preemption between L49 and L55 also exposes it. | Reorder: 1) `_buf[h%N] = r;` 2) `_head.store(newH, release);` 3) update _tail under separate cas/release if needed. OR: writer ALWAYS takes webDataMutex (drop the try-take pattern in ProcessingTask:48). | Pending |
| 12.13 | M | DataPipeline.h:65-72, 86-95 — `copyRecent` / `findLast` / `collectMetricSeries` perform 72-byte memcpy of slots while a concurrent push may overwrite. Per-slot sequence-number guard would let readers retry on torn reads. | Add `std::atomic<uint32_t> _seq[N];` incremented before+after each slot write; reader retries when even/odd parity mismatches. | Pending |
| 12.14 | L | DataPipeline.h:134-135 — comment says "200 entries ≈ 14KB" but actual sizeof(SensorReading) ≈ 72 B → 14.4 KB. Comment correct enough; flag for future-proofing once SensorReading grows. | Replace magic with `WEB_RING_SIZE = (16*1024) / sizeof(SensorReading)`. | Pending |
| 12.15 | I | AggregationEngine.h / LiveAggregator.h / FlowRunLogger.h / DataPipeline.h — interface surfaces; principal risk in `.cpp`. Trivial declarations otherwise. | [MODULE SAFE for headers beyond previously-flagged items] | N/A |

---

