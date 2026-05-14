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

## Phase 13 — IModule Adapters (Network / OTA / Theme)

Files: `modules/WiFiModule.*`, `modules/OtaModule.*`, `modules/ThemeModule.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 13.1 | M | WiFiModule.cpp:8-16 `parseIPv4` — leaves `out[]` untouched on malformed input. User POSTs `{"staticIP":"junk"}` → save returns success, value unchanged. Silent acceptance: UI shows save-success but underlying field never updated. | Return bool; caller (load()) propagates as validation failure → IModule.h:42 contract. Explicit `if (a < 0 \|\| a > 255 \|\| ...)` instead of the bitwise-OR trick at L12. | Pending |
| 13.2 | M | WiFiModule.cpp:42-57 + ThemeModule.cpp:36-53 — `load()` always returns `true` regardless of validation outcome. Out-of-range enums, malformed strings, invalid IPs all silently accepted. IModule.h:42 validation contract unused. Compounds with ModuleRegistry.cpp:78 which discards load() return anyway (already 6.12). | Per field: return false on parse failure; aggregate to caller. Fix 6.12 in tandem so registry honours it. | Pending |
| 13.3 | L | WiFiModule.cpp:44 — `n.wifiMode = (WiFiModeType)(cfg["wifiMode"] \| (int)n.wifiMode);` — no range check on enum. User can set wifiMode=99; downstream code falls into AP-mode fallback without user-visible signal. | Validate `int v; if (v == 0 \|\| v == 1) n.wifiMode = (WiFiModeType)v; else return false;`. | Pending |
| 13.4 | L | WiFiModule.cpp:70-73 — `String(buf)` per IP × 4 = four short-lived heap allocations per save(). ArduinoJson v7 accepts `const char*` directly without the String wrapper. | `cfg["staticIP"] = (const char*)buf;` style — but careful with buffer lifetime; the assignment must happen before `buf` is reused for the next IP. Use a 4-row stack array of buffers. | Pending |
| 13.5 | M | OtaModule + IModule default `_enabled=true` — OTA "enabled" toggle has no semantic effect (no `start()` / `stop()` implementation). UI displays a switch that does nothing; user toggling it sets the boolean in modules.json but nothing in the firmware reads it. | Either implement `start()` to actually arm/disarm OTA route registration, OR override `isEnabled()` to always return true and `setEnabled()` as no-op. | Pending |
| 13.6 | L | OtaModule.cpp:11-18 — `save()` writes live status (running partition, pendingVerify, rollbackCapable) into modules.json on every saveConfig(). State churn pollutes the shadow file; LittleFS rewrites the file even when no user-config actually changed. | Move informational fields out of `save()`; expose them via `/api/modules/ota` GET only (via toDetailJson hook). | Pending |
| 13.7 | H | ThemeModule.cpp:36-53 — Color and path fields (primaryColor, …, lightBgColor, logoSource[129], faviconPath[33], chartLocalPath) accept ANY string with NO validation. A POST `/api/modules/theme` with `{"primaryColor":"javascript:alert(1)"}` or `{"logoSource":"\"><script>...</script>"}` is stored verbatim. Combined with `/export_settings` round-trip back to the UI, stored XSS if the UI ever uses innerHTML/style with the raw value. Defense-in-depth gap. | Validate: color fields must match `^#[0-9a-fA-F]{6}$`; logoSource must be relative path or http(s):// URL; reject `javascript:`/`data:text/html` URIs. | Pending |
| 13.8 | M | ThemeModule.cpp — load/save covers only ~13 of ~20 theme fields. **Missing: ffColor, pfColor, otherColor, storageBarColor, storageBar70Color, storageBar90Color, storageBarBorder, boardDiagramPath.** `/api/modules/theme` cannot edit them while `/save_theme` can. Two endpoints diverge on the same struct. | Add missing fields to both schema string AND load/save; OR delete them from ThemeConfig if truly unused. | Pending |
| 13.9 | L | ThemeModule.cpp:38-41 — enum casts (mode, chartSource, chartLabelFormat) with no range check. `mode=99` stored verbatim → unknown enum value in switch statements downstream. | Validate against known enum values. | Pending |
| 13.10 | L | modules.json shadow vs config.bin precedence — config.bin is authoritative; modules.json shadow rewritten on every saveConfig(). If user manually edits /config/modules.json via /upload, next save silently overwrites. No documented precedence model surfaced to user; restore-from-modules.json path doesn't exist. | Document the model in INSTRUCTIONS.md / settings UI; OR implement two-way sync where modules.json edits trigger config.bin update. | Pending |
| 13.11 | I | WiFiModule.h / OtaModule.h / ThemeModule.h — trivial declarations + Meyers singletons. | [MODULE SAFE for headers] | N/A |

---

## Phase 14 — IModule Adapters (DataLog / Time) + Serial Provisioner

Files: `modules/DataLogModule.*`, `modules/TimeModule.*`, `serial/SerialProvisioner.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 14.1 | M | DataLogModule.cpp:50-71 — `load()` always returns true; same 13.2 family. Out-of-range enums, unbounded numerics, NaN floats silently accepted. | Per-field validation; return false on any rejection; aggregate via ModuleRegistry honouring load() return (tied to 6.12). | Pending |
| 14.2 | M | DataLogModule.cpp:55 — `rotation` enum cast without range check. ROTATION_SIZE=4 is max; `99` stored verbatim, then downstream switch defaults. | Validate against enum range 0..4. | Pending |
| 14.3 | L | DataLogModule.cpp:62-65 — Four uint8_t enum fields (dateFormat/timeFormat/endFormat/volumeFormat) cast from int with no range check. | Validate each against schema option count. | Pending |
| 14.4 | M | DataLogModule.cpp:52-53 — `folder` accepted without `isPathProtected` check. User can POST `{"folder":"_setup"}` via /api/modules/datalog; DataLogger.cpp:60-65 then mkdirs the protected path. Tied to 9.10. | Validate folder against `isPathProtected` and `sanitizePath` on load. | Pending |
| 14.5 | L | DataLogModule.cpp:68-69 — `pfToFfThreshold`/`ffToPfThreshold` accept NaN/Inf via JSON. `applyDefaults` catches at next saveConfig but JSON-direct path bypasses validation. | Add `isfinite()` check; reject or clamp to [0.1, 1000]. | Pending |
| 14.6 | L | TimeModule.cpp:22-23 — narrowing cast `(int8_t)(cfg[...] \| ...)` truncates out-of-range silently. Schema says timezone -12..+14 but JSON `99` is stored as `99` (int8_t holds), then later wraps on math. | Range-check before cast. | Pending |
| 14.7 | M | SerialProvisioner.cpp:144 — `WiFi.begin(ssid, pass)` writes credentials to ESP32 NVS by default. NVS persists across reboots INDEPENDENT of config.bin. Two sources of truth diverge. After `/factory_reset` wipes LittleFS, NVS creds still auto-connect on next boot. | Either `WiFi.persistent(false)` before begin(), OR mirror NVS writes into `config.network.client*` and saveConfig. | Pending |
| 14.8 | M | SerialProvisioner.cpp:134-179 — On successful connect, does NOT update `config.network.clientSSID/clientPassword`. Device runs on NVS creds while config.bin shows stale values; `/export_settings` returns wrong network info; UI confused. | After WL_CONNECTED, write back to config and saveConfig(). | Pending |
| 14.9 | M | SerialProvisioner.cpp:93 — `WiFi.scanNetworks(false, ...)` BLOCKING for 2-4 s. `tick()` runs from main loop (ESP_Logger.ino:745). Blocks OtaManager::tick (90s OTA confirm window can be eaten), SSE publishLiveEvent, all main-loop tickers, watchdog refresh. | Use async scan (`scanNetworks(true)`), poll `scanComplete()` across multiple ticks. | Pending |
| 14.10 | M | SerialProvisioner.cpp + ApiHandlers.cpp:618-647 wifiTestTaskFn — both invoke `WiFi.begin`/`WiFi.mode` without coordination. Concurrent serial-connect + web-wifi-test → undefined radio state, can disconnect the AP serving the request. | Add a single `g_wifiOpInFlight` flag; reject concurrent ops with 409. | Pending |
| 14.11 | L | SerialProvisioner.cpp:33-35 — Overflow path silently resets buffer; no error response. Host has no signal that a long command was truncated. | Emit `{"ok":false,"err":"line_too_long"}` then reset. | Pending |
| 14.12 | L | SerialProvisioner runs unconditionally every loop iteration (ESP_Logger.ino:745) on every boot — even in deployed devices with USB plugged into a power adapter. Extra UART/USB-CDC attack surface (physical-access required, but any access can drive WiFi mode changes). | Gate via `setup.h` macro `SERIAL_PROVISIONER_ENABLED` (off by default in non-dev builds). | Pending |
| 14.13 | I | DataLogModule.h / TimeModule.h / SerialProvisioner.h — trivial declarations + Meyers singletons. | [MODULE SAFE for headers] | N/A |

---

## Phase 15 — Sensor Framework & Alerts

Files: `sensors/ISensor.h`, `sensors/SensorManager.*`, `alerts/AlertEngine.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 15.1 | L | ISensor.h:11-22 `CalibrationAxis::load` — no validation that `scale != 0` or `!isnan(offset/scale)`. User config `{"scale":0}` produces constant `offset` for every reading; `{"scale":"NaN"}` poisons every read. | Reject scale==0 and non-finite values; fall back to defaults. | Pending |
| 15.2 | I | ISensor.h:90 `setId` — `strncpy(_id, id, sizeof(_id)-1)` relies on `_id[]` being zero-initialised by the protected default-init (`char _id[17] = {}`). Safe today; brittle if subclass adds custom ctor that skips the brace-init. | Add an explicit `_id[sizeof(_id)-1] = '\0';` after strncpy. | Pending |
| 15.3 | H | SensorManager.cpp:39 + 195-197 — `reloadConfig` calls `_destroyAll()` (frees `_sensors[]`) while SensorTask iterates the same array WITHOUT configMutex on the reader side. Use-after-free on the freed plugin pointers. Restates 3.19 with the producer-side context. | Acquire configMutex around `tickFiltered` body; OR signal a quiesce flag SensorTask honours before destroyAll. | Pending |
| 15.4 | M | SensorManager.cpp:126-128 — `wireMutex` acquire with 100 ms timeout; on failure `tookMutex=false` and code PROCEEDS to `s->readAll()` WITHOUT lock. Silent fallback to unlocked I2C bus → bus contention with concurrent plugins. | On mutex timeout, skip this sensor's read for the tick; increment a `g_busSkips` counter. | Pending |
| 15.5 | L | SensorManager.cpp:74 — `if (!sensor["enabled"]) continue;` defaults to FALSE when the JSON key is missing. User omitting `enabled` silently disables the sensor. | Default to true: `bool en = sensor["enabled"] \| true;`. | Pending |
| 15.6 | M | SensorManager.cpp:281 + 326-364 — `toJson` builds the per-sensor skeleton BEFORE taking webDataMutex; if mutex acquire fails (50 ms timeout), function early-returns leaving rules + skeleton fields populated but `last_values`/`spark`/`health` blocks missing. UI receives a partial response with no error signal. | On mutex-take failure, emit `o["partial"] = true` per sensor so UI can flag stale data. | Pending |
| 15.7 | M | AlertEngine.cpp:139 — `evaluate` uses `xSemaphoreTake(_mutex, 0)` (non-blocking). On contention with toJson/fromJson/snooze, evaluation is SILENTLY SKIPPED. Sensor readings during web-API activity miss alert checks; no counter exposed. | Use short timeout (5 ms) and increment `g_alertEvalDrops` on failure. | Pending |
| 15.8 | H | AlertEngine.cpp:192-218 `_dispatch` — calls `g_mqttExporter->send(&ar, 1)` WHILE HOLDING `_mutex`. `send()` does TLS network I/O blocking seconds; violates the L185 contract "no blocking I/O". Also risks deadlock if MqttExporter ever takes its own mutex in evaluate path. | Queue the alert into a side ring; have a separate task drain it without holding _mutex. | Pending |
| 15.9 | M | AlertEngine.cpp:402-406 `_save` — `_fs->open(_path, FILE_WRITE)` truncates immediately; non-atomic write. Power loss mid-save = corrupt alerts.json. No fsMutex either. Same class as 8.6 / 9.8 / 6.14. | Write to `.tmp`, then atomic rename. Acquire fsMutex around the open/serialize/close/rename block. | Pending |
| 15.10 | M | AlertEngine.cpp:155-156 — `(rule.duration_s == 0) \|\| ((nowTs - rule.condFirstMetTs) >= rule.duration_s)`. When nowTs=0 (SensorTask fallback per 10.3) and condFirstMetTs=0, subtraction=0, condition immediately satisfied → false-positive trigger on pre-NTP readings. Tied to 11.4. | Skip evaluate when `nowTs < 1000000000` (no wall clock). | Pending |
| 15.11 | L | AlertEngine.cpp:69, 331 — `ALERT_MAX_RULES=8` silently truncates oversized rule arrays at parse. UI POST with 10 rules loses the 9th and 10th with no error. | Return false from fromJson when input exceeds cap; surface 413 to client. | Pending |
| 15.12 | M | AlertEngine.cpp:319-340 `fromJson` — sets `_ruleCount = 0` BEFORE parsing, then commits via `_save()`. If JSON body has missing/empty `rules` array (e.g. UI sends `{}`), ALL existing rules wiped without warning. No separate DELETE endpoint exists, so a malformed PUT body silently destroys the rules. | Parse into a temp `Rule[ALERT_MAX_RULES]`, only commit + save if successful AND newRuleCount > 0 (unless explicit `{"rules":[]}` opt-in). | Pending |

---

## Phase 16 — Storage Backends + RTC Driver

Files: `storage/CsvLogger.*`, `storage/HybridStorage.*`, `drivers/DS1302_Mini.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 16.1 | H | CsvLogger.cpp:67-76 `_rotate` — `_fs->remove(bak); _fs->rename(path, bak);` non-atomic. Power loss in the gap leaves NEITHER file present (primary CSV gone). Same class as 9.8, 12.11. | LittleFS rename overwrites atomically — drop the explicit `remove(bak)` call. | Pending |
| 16.2 | M | HybridStorage entire module is DEAD CODE — `begin()`/`primary()`/`secondary()`/`mirrorWrite()` are never called from anywhere (verified via grep across src/ and ESP_Logger.ino). Mirror functionality lives instead in TaskManager.cpp:115-126. Module is linked into firmware (~2-3 KB flash bloat) and misleads maintainers into thinking it's the active mirror path. | Delete `src/storage/HybridStorage.*` OR wire it into TaskManager to replace the inline mirror block. | Pending |
| 16.3 | L | HybridStorage.cpp:39 — `SD.begin(_sdCS)` with default `pinSdCS=10` collides with C3 SPI flash pin range (per 5.8). Dead code mitigates impact, but if revived would inherit 5.8. | Tied to 5.8; document or fix in tandem. | Pending |
| 16.4 | L | HybridStorage.cpp:69-90 `mirrorWrite` — no `fsMutex` acquire; concurrent caller would race against StorageTask, ConfigManager, etc. Dead code today; flagged if revived. | Acquire fsMutex around the open/write/close blocks. | Pending |
| 16.5 | L | DS1302_Mini.h:226 — `SetDateTime` writes `_dec2bcd(year - 2000)`. For year ≥ 2100, `(100/10)<<4 \| 100%10 = 0xA0` is invalid BCD. DS1302 hardware behavior on invalid BCD is unspecified; may corrupt time. | Clamp year to 2000..2099, or return error for out-of-range. | Pending |
| 16.6 | I | DS1302_Mini.h:137-159 — `writeByte`/`readByte` bit-banged at ~1 MHz (delayMicroseconds(1) per phase). Well within DS1302 max SCLK spec. Acceptable. | [No action] | N/A |
| 16.7 | I | CsvLogger.h / HybridStorage.h — trivial header surfaces; principal risk in `.cpp`. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 17 — Mini Drivers (MQTT, BME280, BME688, DS18B20)

Files: `drivers/MQTT_Mini.h`, `drivers/BME280_Mini.h`, `drivers/BME688_Mini.h`, `drivers/DS18B20_Mini.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 17.1 | M | MQTT_Mini.h:63 — On `remaining + 5 > sizeof(buf)` overflow, sets `_state=-4` and returns false WITHOUT calling `_tcp->stop()`. TCP connection to broker leaks; next connect attempt may collide. | Add `_tcp->stop();` before return false at L63. | Pending |
| 17.2 | H | MQTT_Mini.h — NO TLS support. Credentials + sensor data sent plaintext on port 1883. For non-LAN deployments (e.g. cloud broker over internet) every payload + auth is sniffable. | Add `WiFiClientSecure` variant + setCACert path; expose via setup.h flag `EXPORT_MQTT_TLS_ENABLED`. | Pending |
| 17.3 | M | MQTT_Mini.h:98-105 — CONNACK wait blocks calling task up to 5 s (`delay(10)` × 500). Called from ExportTask normally (acceptable); if ever invoked from AsyncTCP worker (alert path 15.8) the whole web stalls. | Use non-blocking poll with FreeRTOS task yield; document the blocking contract on the function. | Pending |
| 17.4 | L | MQTT_Mini.h:122 — `publish()` accepts arbitrary uint16_t topic+payload lengths (up to 65 535). WiFiClient internal TCP buffer is typically 4-5 KB; oversized payload gets chunked but each `_tcp->write` may short-write. | Add `if (topicLen + payloadLen > 4096) return false;` guard. | Pending |
| 17.5 | M | BME280_Mini.h:36 — `while ((_read8(0xF3) & 0x01) != 0) delay(1);` — wait for NVM copy with NO timeout. If chip hangs (broken sensor, I2C noise), infinite loop hangs boot. | Add `for (int i = 0; i < 100; i++) { if (... == 0) break; delay(1); }` then return false on timeout. | Pending |
| 17.6 | M | BME280_Mini.h:66, 88 — `readPressure()` and `readHumidity()` rely on `_t_fine` set by previous `readTemperature()` call. No assertion or guard; out-of-order calls produce garbage values. Plugin code must always call temp first. | Make readPressure/Humidity call readTemperature internally if `_t_fine == 0`; or expose `readAll()` that sequences them. | Pending |
| 17.7 | L | BME280_Mini.h:160-204 — `_read8`/`_read16`/`_read24`/`_readBlock` discard `Wire.endTransmission()` return code AND don't verify `requestFrom` count matches. I2C error returns 0xFF on the bus → silently parsed as normal data. | Check `endTransmission != 0` and `requestFrom == len`; on error return sentinel NaN; have plugin convert to QUALITY_ERROR. | Pending |
| 17.8 | M | BME688_Mini.h:167-194 — Calibration register decoding uses magic indices into coeff1[]. Comments at L168-170 admit re-derivation from Bosch datasheet. No tests vs official BSEC reference; gas resistance compensation may be miscalculated for some sensors. | Cross-check every coefficient index against Bosch BME68x driver source; add a self-test against a known reading. | Pending |
| 17.9 | L | BME688_Mini.h:269-282 `_calcHeaterRes` — int32 intermediate arithmetic can overflow at max target temperature (400 °C). Bosch's own reference driver carries the same risk. Heater calibration may be off-by-N for high-temp settings. | Promote to int64 for the var2 calculation. | Pending |
| 17.10 | M | BME688_Mini.h:83-88 — `performReading` blocks up to 1 s polling status register. Plugin must declare `isBlocking()=true`; if not, SensorTask path stalls every other sensor. To verify in Phase 20. | Verify BME688Sensor::isBlocking() returns true. | Pending |
| 17.11 | H | DS18B20_Mini.h:134-143 `_readBit` — Critical timing window (`delayMicroseconds(3) + delayMicroseconds(10) + delayMicroseconds(53)`) executed WITHOUT `noInterrupts()` / `portDISABLE_INTERRUPTS()`. FreeRTOS scheduler tick (1 ms on ESP32-C3 default) preempting between assert and sample corrupts the bit → CRC failure at best, wrong temperature at worst. 1-Wire is timing-critical. | Wrap each `_writeBit`/`_readBit` (and ideally each 8-bit byte op) in `portDISABLE_INTERRUPTS()` / `portENABLE_INTERRUPTS()`. Note: also disables WiFi ISRs briefly; should be acceptable for ~70 μs windows. | Pending |
| 17.12 | L | DS18B20_Mini.h:38 — Config register write `((res-9)&3)<<5 \| 0x1F` leaves bit 7 in a non-spec state (datasheet "reserved must be 1"). Most chips tolerate; some clones may not. | Use `0x9F` (sets bit 7) instead of `0x1F`. | Pending |
| 17.13 | M | DS1302_Mini.h ThreeWire — bit-banged access from multiple call sites (main loop, SensorTask, SlowSensorTask, StorageTask, WebServer handlers) with NO mutex protecting shared CE/IO/SCLK pins. Concurrent reads of the time registers can interleave at the bit level, returning garbage. Less critical than 17.11 (no scheduler-preemption hazard within a single read window because RTC reads are slow), but inter-task races corrupt the burst-read. | Add a `rtcMutex` semaphore; require every Rtc->* call to take it (timeout 100 ms). | Pending |
| 17.14 | L | MQTT_Mini.h general — Plain MQTT 3.1.1 publish-only; no SUBSCRIBE / SUB-ACK handling. Documented "publish-only" design. Future RPC features blocked. | Document explicitly in header. | N/A |
| 17.15 | I | Mini-driver headers are inline implementations; risk surface fully audited in this phase. Caller (plugin) must check return codes and call sequence. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 18 — Export Framework

Files: `export/IExporter.h`, `export/ExportManager.*`, `export/HttpExporter.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 18.1 | H | ExportManager.cpp:68-72 `_sendWithRetry` — exponential backoff: default 3 retries × 5 s base = 5+10+20 = **35 s blocking** per exporter. With 5 exporters enabled = up to **175 s blocked**. ExportTask heartbeat starves; C4 watchdog (30 s) fires false-positive reset. Tied to 11.8. | Cap total retry budget per sendAll call to ~15 s; refresh heartbeat between retries; or move retry to a state-machine across multiple ticks. | Pending |
| 18.2 | M | ExportManager.cpp:165-176 `_drainSpool` — On a batched failure (line 165 break), spool file is preserved. Batches already successfully sent in earlier iterations are NOT removed from the file. Next drain re-reads same lines → duplicate sends. Non-idempotent exporters (HTTP POST with side effects) see duplicates. | Track byte offset of last successfully-sent line; rewrite spool with only un-sent tail; or use a 2-file rotation (in-flight / pending). | Pending |
| 18.3 | M | ExportManager.cpp:86-123 `_spoolBatch` — opens `/spool/<name>.jsonl` for append without `fsMutex`. Race against ConfigManager, StorageTask, RtcManager.backupBootCount, AlertEngine._save, etc. Same family as 8.8 / 9.9 / 9.12 / 12.10 / 15.9. | Acquire fsMutex around the open/write/close block. | Pending |
| 18.4 | M | ExportManager.cpp:97-106 — Size-cap check then append is TOCTOU race. Two concurrent `_spoolBatch` calls can both pass the cap check and both append, exceeding MAX_SPOOL_BYTES. | Single read of file size under fsMutex; abort if over cap before opening for write. | Pending |
| 18.5 | L | ExportManager.cpp:114-117 `r[i].toJsonLine(line, sizeof(line))` — writes UNESCAPED strings (5.17). Sensor IDs containing `"` or `\` produce malformed JSON lines; later `_drainSpool::deserializeJson` (L152) silently drops them with `continue`. Data loss without user signal. | Fix 5.17 at SensorTypes.h level; or escape at toJsonLine call site here. | Pending |
| 18.6 | L | ExportManager.cpp:174 — `_spoolFS->remove(path);` no fsMutex. Race vs concurrent `_spoolBatch` append. | Acquire fsMutex around the remove call. | Pending |
| 18.7 | L | IExporter.h:27-28 — `maxRetries() = 3`, `retryDelayMs() = 5000` as defaults. Combined with sequential per-exporter dispatch, cumulative blocking exceeds C4 watchdog window. Tuning required. | Defaults: `maxRetries=1`, `retryDelayMs=2000`. | Pending |
| 18.8 | H | HttpExporter.cpp:14-21 — User-supplied header values copied verbatim via `strncpy(_hdrVals[...], kv.value().as<const char*>() ?: "", ...)`. NO CRLF stripping. A platform_config.json entry like `"X-Foo":"a\r\nHost: evil.com"` injects extra headers into the outbound HTTPClient request → **HTTP header injection / request smuggling**. | Reject any header value containing `\r`, `\n`, or `\0`. | Pending |
| 18.9 | H | HttpExporter.cpp:51 — `http.begin(_url)` accepts `https://` URLs but NO certificate verification configured. HTTPClient defaults to insecure mode in this construction; no `setCACert`/`setInsecure` exposed. **Plaintext credentials / MITM risk** on the exporter's HTTP traffic. Same applies to WebhookExporter, SensorCommunityExporter, OpenSenseMapExporter. | Add `setCACert` config option; or `setInsecure()` with an explicit opt-in macro for dev builds; fail closed by default on HTTPS URLs without cert. | Pending |
| 18.10 | M | HttpExporter.cpp:33-35 — `new char[bodyLen]` without `std::nothrow`. On heap pressure → `bad_alloc` → abort. Same family as ApiHandlers.cpp:236. | `new(std::nothrow) char[bodyLen]`; on nullptr return false. | Pending |
| 18.11 | M | HttpExporter.cpp:42-46 — Body built via snprintf `%s` for sensorId/sensorType/metric/unit with NO JSON escaping. Same JSON injection class as 5.17 / 18.5. Malicious sensor id in platform_config.json corrupts the POST body. | JSON-escape strings before snprintf. | Pending |
| 18.12 | I | IExporter.h interface is clean; risk surface fully delegated to implementations. | [MODULE SAFE] | N/A |

---

## Phase 19 — Cloud Exporters (MQTT / Webhook / SensorCommunity / OpenSenseMap)

Files: `export/MqttExporter.*`, `export/WebhookExporter.*`, `export/SensorCommunityExporter.*`, `export/OpenSenseMapExporter.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 19.1 | M | MqttExporter.cpp:79-82 `send()` — On single-reading publish failure, sets `allOk=false` but CONTINUES publishing remaining readings, then returns false. ExportManager's retry path resends the WHOLE batch → already-published readings re-published. Idempotency violation. | On first failure: break early, mark allOk=false, return immediately. Caller's retry resends only unsent tail. | Pending |
| 19.2 | H | MqttExporter — Plaintext MQTT only (via MQTT_Mini at port 1883). Restates 17.2 from exporter perspective. Username/password sent in clear in CONNECT packet; subsequent PUBLISH payloads in clear. | Add TLS variant (WiFiClientSecure) for cloud broker deployments. | Pending |
| 19.3 | M | MqttExporter.cpp:13-22 — `broker`/`topicPrefix`/`clientId`/`username`/`password` copied via strncpy from JSON without sanitization. CONNECT packet builder in MQTT_Mini reads these verbatim. A `topic_prefix` with `\0` or non-printable bytes corrupts every PUBLISH topic. | Reject non-printable chars (< 0x20 or 0x7F) in topic-related fields. | Pending |
| 19.4 | L | MqttExporter.cpp:166 — `_publishDiscoveryOne(s->getId(), s->getName(), mNames[m], "", dc)` passes **empty unit** literal. HA discovery payload omits `unit_of_measurement` → HA sensors render raw numbers without units. | Extend ISensor to expose getMetricUnit(metric) → pass it through to discovery. | Pending |
| 19.5 | L | MqttExporter.cpp:170 — `delay(20)` per metric publish during HA discovery. With 8 sensors × 4 metrics = 640 ms blocking. Runs once at boot via ESP_Logger.ino:442; OK there, but exposed via `/api/mqtt/ha_discovery` POST (ApiHandlers.cpp:438-449) where it blocks the AsyncTCP worker. | Replace `delay` with `vTaskDelay`; for the API path, schedule discovery to run from loop() via a flag. | Pending |
| 19.6 | L | WebhookExporter.cpp:18-19 — `condition` field defaults silently to `"above"` when not exactly `"above"` or `"below"`. A typo like `"abve"` is accepted with no validation error. | Reject unknown condition strings; return false from init() per IModule contract. | Pending |
| 19.7 | M | WebhookExporter.cpp:60-78 — Sequential HTTP POSTs blocking ExportTask. Multiple rule breaches in one batch fire sequentially; each POST blocks ~500 ms. C4 starvation amplified per 11.8. | Cap firings per batch to 1; queue rest for next sendAll cycle. | Pending |
| 19.8 | M | WebhookExporter.cpp:35-43 — Body built via snprintf `%s` for sensor_id/metric with NO JSON escaping. Same injection class as 18.11. | JSON-escape strings before snprintf. | Pending |
| 19.9 | H | WebhookExporter.cpp:31 — `http.begin(_url)` HTTPS with no cert verification. Same as 18.9. | Tied to 18.9. | Pending |
| 19.10 | M | WebhookExporter as a whole — semantically OVERLAPS with AlertEngine (Phase 15). Two parallel threshold-alert systems with different storage (RAM rules in webhook config vs alerts.json), different delivery channels (HTTP-only here, MQTT/toast in alerts), and no coordination. Double-firing if same threshold set in both. Confusing UX. | Choose one canonical alert engine; refactor WebhookExporter to consume AlertEngine fired events via a hook. | Pending |
| 19.11 | H | SensorCommunityExporter.cpp:24 — `http.begin(API_URL)` HTTPS without cert verification. Same as 18.9. | Tied to 18.9. | Pending |
| 19.12 | L | SensorCommunityExporter.cpp:47-54 — Picks the FIRST occurrence of each metric across all readings in the batch. Multi-sensor setups with two BME280s upload only one sensor's data. | Either upload each sensor as a separate POST, or aggregate (avg) across same-metric readings. | Pending |
| 19.13 | L | SensorCommunityExporter.cpp:80 — `pres * 100.0f` assumes plugin emits pressure in hPa. Plugin emitting Pa (BME280_Mini.h:85 returns Pa directly per Bosch formula) would be multiplied AGAIN by 100 → 100× wrong upload. Tied to 11.2 (no unit-aware contract). | Inspect r.unit and convert based on it; or document hPa requirement. | Pending |
| 19.14 | H | OpenSenseMapExporter.cpp:9, 73 — `access_token` stored plaintext in platform_config.json; sent in `Authorization: Bearer ...` header. Same `/export_settings` exposure as WiFi creds (3.7 family). Plus HTTPS to api.opensensemap.org without cert verification (18.9 family). Combined: token visible via /download AND vulnerable to MITM. | (a) Mask token in /export_settings response; (b) require explicit `?include_secrets=1` opt-in. (c) Add cert verification per 18.9. | Pending |
| 19.15 | M | OpenSenseMapExporter.cpp:44-46 — `new char[bodyLen]` without `std::nothrow`. Same family as 18.10. | `new(std::nothrow)`. | Pending |
| 19.16 | L | OpenSenseMapExporter.cpp:67 — `snprintf(url, 128, "%s%s/data", API_BASE, _boxId);` — `_boxId` from user config is concatenated into URL path without validation. Malicious boxId like `abc/../../../other-box` could redirect uploads (HTTPClient may or may not normalize). | Validate _boxId against `^[a-f0-9]{24}$` (openSenseMap ID format). | Pending |
| 19.17 | I | All exporter `init()` paths copy raw user-supplied strings into char arrays via strncpy without explicit NUL termination — works only because the receiving buffers are zero-initialized at declaration. Brittle if a future refactor adds non-trivial constructors. | Add explicit `_buf[sizeof(_buf)-1] = '\0';` after every strncpy. | Pending |

---

## Phase 20 — Environmental Sensor Plugins (BME280 / BME688 / DS18B20)

Files: `sensors/plugins/BME280Sensor.*`, `BME688Sensor.*`, `DS18B20Sensor.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 20.1 | M | BME280Sensor.cpp:12-16 + BME688Sensor.cpp:12 (+ all I2C plugins) — Every sensor's `init()` calls `Wire.begin(sda, scl)` if pins are specified. If two sensors in platform_config.json specify **different** SDA/SCL, the LAST init wins; previously-initialised sensors now address the wrong bus. Bus reconfiguration race. | Centralise I2C bus setup once at boot (e.g. `HardwareManager::initI2C` reading first sensor's pins); plugins should NOT call Wire.begin themselves. | Pending |
| 20.2 | M | BME688Sensor.cpp:47 — `if (!_ready \|\| maxOut < 4) return 0;` — strict 4-metric requirement. Caller with maxOut=3 (e.g. when one slot was used by another sensor in the same tick) drops ALL data instead of returning the 3 available. Wastes the entire forced-mode read. | Write up to `min(4, maxOut)` metrics; return that count. | Pending |
| 20.3 | M | DS18B20Sensor.cpp:36 — `_ready = true` set when `count == 0` with comment "bus may have device connect later." False premise: `DS18B20_Mini._count` is set ONLY inside `begin()`. Hot-plugged sensors are never detected without a manual re-init. Comment misleads. | Either remove the "_ready=true" path (return false on no devices), OR add a `rescan()` method called periodically by readAll. | Pending |
| 20.4 | L | BME280Sensor.cpp:62 vs 71-73 — Inconsistent use of `_makeReading` helper at L62-63 vs direct `SensorReading::make` at L71-73. Maintainer confusion; both produce the same output today. | Use one pattern. | Pending |
| 20.5 | L | DS18B20Sensor.cpp:59-60 — `requestTemperatures(); delay(conversionTimeMs());` blocks SlowSensorTask up to 760 ms. Acceptable per `isBlocking()=true`, but the subsequent `getTempC()` reads in `DS18B20_Mini` carry the 17.11 noInterrupts timing risk. | Tied to 17.11; fix in driver level. | Pending |
| 20.6 | I | All I2C sensor plugins rely on SensorManager wrapping their reads with `wireMutex` (15.4 family). `init()` paths run at boot single-threaded (loadAndInit calls per sensor sequentially) so init-time bus access is safe; runtime reads are guarded by SensorManager. OK. | [Acceptable] | N/A |
| 20.7 | I | Headers (BME280Sensor.h, BME688Sensor.h, DS18B20Sensor.h) — trivial declarations + getMetrics with static const arrays returned. | [MODULE SAFE for headers] | N/A |

---

## Phase 21 — Air Quality Sensor Plugins (SDS011 / PMS5003 / ENS160 / SGP30)

Files: `sensors/plugins/SDS011Sensor.*`, `PMS5003Sensor.*`, `ENS160Sensor.*`, `SGP30Sensor.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 21.1 | H | SDS011Sensor.cpp:26 + PMS5003Sensor.cpp:15 — BOTH set `_serial = &Serial1` and call `Serial1.begin(baud, SERIAL_8N1, rx, tx)`. ESP32-C3 has only ONE hardware UART besides USB-CDC (Serial1). If user enables both in platform_config.json, the second init reconfigures Serial1 (different baud and pins), **silently breaking the first**. Same UART cannot serve two devices simultaneously. | Mutually exclusive: refuse to register the second if the first is already on Serial1; OR document UART-share limitation in INSTRUCTIONS.md. | Pending |
| 21.2 | M | SDS011Sensor.cpp:67-74 `_drainBuffer` — 100 ms busy loop in `init()` (called at boot from `_initPlatform`). Combined with other sensor init delays, contributes to multi-second boot stretch BEFORE OtaManager::tick begins (compounds 1.14 / 8.10 / 9.x ordering risks). | Replace `delay(10)` with `vTaskDelay`; cap drain at 50 ms. | Pending |
| 21.3 | L | SDS011Sensor.cpp:37-45 — Hardware Working Period config command sent fire-and-forget; NO ACK frame check. Sensor may silently refuse to switch modes; user thinks `work_period_min=5` is active when sensor stays continuous. | Read response frame within 1 s; verify checksum + work-period byte; log warning on mismatch. | Pending |
| 21.4 | L | SDS011Sensor.cpp:36-45 — Command checksum sums bytes 2..16 (line 38-39) per Nova Fitness spec. Buffer initializer at L37 sets cmd[17] to 0 but is then OVERWRITTEN by checksum at L40. Correct, but the read-once initializer is dead. Cosmetic. | Drop the `0` placeholder in initializer (use `{}` rest). | Pending |
| 21.5 | M | PMS5003Sensor.cpp:28-39 `_readFrame` — 2 s deadline loop with `delay(5)` and NO `g_taskHeartbeat[TASK_IDX_SLOW_SENSOR]` refresh. SDS011's L115 DOES refresh during a similar wait. Inconsistent; if MAX_SILENCE_MS were ever tightened below 30 s, PMS5003 reads would trip the C4 watchdog. | Add `g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();` at top of the loop body. | Pending |
| 21.6 | M | ENS160Sensor.cpp:64-81 `readAll` — Does NOT call `_waitReady` (NEW_DATA bit) before reading TVOC/eCO2 registers. If sensor isn't fully ready (warmup, transient), reads stale/garbage values that pass `isPlausible` (tvoc 0..65535, eco2 400..65535). | Call `_waitReady(200)` at top; on failure return 0; OR mark readings as QUALITY_ESTIMATED. | Pending |
| 21.7 | L | ENS160Sensor.cpp:79 — AQI emitted as float with unit "". ProcessingTask `isPlausible` has no AQI check → catch-all returns true. Garbage AQI (>5) passes validation and reaches AlertEngine. | Add bounds check `if (aqi < 1 \|\| aqi > 5)` → mark QUALITY_ERROR; add `aqi` case to ProcessingTask.isPlausible. | Pending |
| 21.8 | L | SGP30Sensor.cpp:43-78 `init` — Sequence: get_feat → IAQ_INIT. After IAQ_INIT, first 15 s of readings are baseline-only. Class correctly marks these as `QUALITY_ESTIMATED` (L96). But the comment in the header (L11-13) says "first 15 readings return baseline" — at 30 s interval, that's 7.5 minutes of estimated readings. Doc/code mismatch. | Update comment: "First reading after init returns estimated; switches to GOOD after 15 s warmup elapses." | Pending |
| 21.9 | M | SGP30Sensor.cpp:33-41 `_measure` — On I2C failure (single transient noise byte → CRC mismatch in `_readWords`), returns false → `readAll` returns 0. NO retry within one call. Combined with SensorTask interval, one transient noise burst skips an entire 30 s cycle. | Retry once after 10 ms on CRC failure. | Pending |
| 21.10 | L | SDS011Sensor.cpp:113-115 — `g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();` manual refresh inside `read()` and `readAll()` blocking waits. Direct write to a global volatile array from sensor plugin code violates encapsulation (the heartbeat is a TaskManager concern). Acceptable today since plugins know they're on SlowSensorTask. | Provide a `SensorPlatform::tickHeartbeat()` helper that hides the array. | Pending |
| 21.11 | I | All UART/I2C plugin headers — trivial declarations, static metric arrays, calibration members. | [MODULE SAFE for headers] | N/A |

---

## Phase 22 — CO2 & Light Sensor Plugins (SCD4x / VEML6075 / VEML7700 / BH1750)

Files: `sensors/plugins/SCD4xSensor.*`, `VEML6075Sensor.*`, `VEML7700Sensor.*`, `BH1750Sensor.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 22.1 | H | SCD4xSensor.cpp:63-69 — `_sendCmd(CMD_START_PERIODIC); delay(5100);` blocks 5.1 s in `init()`. With 4+ sensors at staggered init delays, total boot time can exceed OtaManager::boot arm window (tied to 1.14 / 8.10). Pending-verify firmware that crashes before sensor init finishes never gets rolled back. | Don't `delay(5100)` — set `_ready=false` and let first readAll's `_dataReady()` gate. Or move OtaManager::boot earlier per 1.14. | Pending |
| 22.2 | M | SCD4xSensor.cpp:70 — `_ready = true` set after CMD_START_PERIODIC send succeeds with NO confirmation the device is actually in periodic mode. Subsequent `_dataReady()` may always return false; reads silently fail. | After init delay, check `_dataReady()`; refuse to mark `_ready=true` on failure. | Pending |
| 22.3 | M | SCD4xSensor.cpp:94 — `co2 = _calCo2.apply((float)words[0]);` — raw uint16 (0..65535) silently accepted. SCD40 max 2000 ppm, SCD41 max 5000 ppm. ProcessingTask `isPlausible` has no `co2` case (11.1) → garbage reaches AlertEngine. | Range-check `400 <= words[0] <= 5000`; add co2 to isPlausible (fix 11.1 in tandem). | Pending |
| 22.4 | H | VEML6075Sensor.h:51 + VEML7700Sensor.h:55 — BOTH use I2C ADDR=0x10 (fixed, no override). The two cannot coexist on the same bus. No conflict detection; user enabling both gets silent device-confusion (whichever device ACKs the address services every read/write). | Reject second plugin registration with the same fixed address; surface via Serial + sensor.status="error". | Pending |
| 22.5 | M | VEML7700Sensor.cpp:97 + L100-106 — `lux = _calLux.apply(als * _resolution)` at L97. Then L100-104 high-lux non-linear correction recomputes from the already-calibrated value, and L105 calls `_calLux.apply(lux)` AGAIN. Result: calibration **offset added twice, scale squared** for lux > 1000. | Apply non-linear correction to raw `als*_resolution` BEFORE calibration: `if (rawLux > 1000) rawLux = poly(rawLux); lux = _calLux.apply(rawLux);`. | Pending |
| 22.6 | M | VEML7700Sensor.cpp:90-95 — `readAll` bails on ANY register read failure (line 94 or 95 returns 0). Single transient I2C error skips a whole 5 s read cycle. Same class as 21.9 / 22.x. | Retry once after 10 ms before bailing. | Pending |
| 22.7 | L | VEML6075Sensor.cpp:39-42 — Device-ID mismatch logged but ignored ("continue anyway — some modules don't expose ID"). Allows misconfigured sensors at other addresses to silently report garbage. | Make strict-ID opt-out via config flag (default opt-IN). | Pending |
| 22.8 | L | BH1750Sensor.cpp:42-49 — `_sendCmd(CMD_RESET)` and `_sendCmd(_modeCmd)` return values DISCARDED. If RESET or mode-set fails after POWER_ON, init proceeds and `_ready=true`. First read may return garbage. | Check returns; on any failure return false. | Pending |
| 22.9 | I | All Phase-22 plugins call `Wire.begin(sda, scl)` in init — inherits 20.1 race. | See 20.1. | N/A |
| 22.10 | I | SCD4xSensor.h, VEML6075Sensor.h, VEML7700Sensor.h, BH1750Sensor.h — trivial declarations + static metric arrays. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 23 — Flow & Weather Sensor Plugins (WaterFlow / YFS201 / Rain / Wind)

Files: `sensors/plugins/WaterFlowSensor.*`, `YFS201Sensor.h`, `RainSensor.*`, `WindSensor.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 23.1 | C | RainSensor.cpp:24 + RainSensor.h:44 — Default `_pin=9` with `pinMode(_pin, INPUT_PULLUP)`. **GPIO9 is an ESP32-C3 boot-strap pin** — held LOW at power-on selects SPI download mode. A typical tipping-bucket reed switch pulls to GND when closed; if the bucket happens to be in the tipped state at power-on, the device boots into download mode → soft-brick until next manual reboot. | Change default `_pin` to a non-strap GPIO (e.g. 4 or 10); document strap-pin avoidance prominently in INSTRUCTIONS.md and the sensor schema. | Pending |
| 23.2 | M | YFS201Sensor.h — Header file for a class with **NO `.cpp` implementation**. ESP_Logger.ino:387-388 registers `"yfs201"` via the `WaterFlowSensor` factory lambda, NOT `YFS201Sensor`. The header would only matter if some code path called `new YFS201Sensor`, which would then fail to link. Dead code; misleads future maintainers. | Delete `src/sensors/plugins/YFS201Sensor.h`. | Pending |
| 23.3 | H | RainSensor.cpp:25-28 + WindSensor.cpp:30-33 — Same `gpio_isr_handler_add(pin, _isr, this)` without destructor cleanup as 2.6. `SensorManager::reloadConfig → _destroyAll → delete _sensors[i]` frees the object; next pulse fires the IRAM ISR on a dangling `this` → crash. Extends 2.6 to two more sensor classes. | Add `~RainSensor()` / `~WindSensor()` calling `gpio_isr_handler_remove((gpio_num_t)_pin);`. | Pending |
| 23.4 | M | WindSensor.cpp:61 — `delay(_sampleWindowMs)` defaults to 3000 ms; `getReadIntervalMs() = _sampleWindowMs`. Net duty cycle = 100% — SlowSensorTask spends every 3 s blocked on wind reads, starving SDS011/PMS5003 (same task). | Decouple sample window from poll interval (e.g. sample 1× per 30 s), or cap window at 1 s default. | Pending |
| 23.5 | M | WindSensor.cpp:53-86 — `_pulses = 0` at L58 then `delay(3000)` at L61 with interrupts ENABLED, then `count = _pulses` at L64. A pulse arriving between L64 read and L72 use is NOT counted in THIS sample but will be the next. Subtle off-by-one drift in continuous wind. | Acceptable for low-precision wind; document or use atomic snapshot at start AND end of window. | Pending |
| 23.6 | L | WindSensor.cpp:76 — `analogRead(_dirPin)` single sample mapped directly to angle. Noisy ADC produces jittery wind direction. | 8-sample average; or median-of-3. | Pending |
| 23.7 | L | RainSensor.cpp:54-55 — Instantaneous `rain_rate` computed from a single inter-tip interval. One tip in 30 min produces SAME rate as 3 tips in 30 min. | Accumulate tip count over a rolling window; `rate = window_tips × mm_per_tip × (3600 / window_sec)`. | Pending |
| 23.8 | I | WaterFlowSensor.cpp:39-41 — ISR install pattern flagged in 2.6 / 17.x; restated as the 23.3 fix scope must also include WaterFlowSensor's destructor. | See 2.6 / 23.3. | Pending |
| 23.9 | I | All ISR-driven sensors share the static-bool `_isrServiceInstalled` pattern (WaterFlow L39, Rain L26, Wind L31). Idempotent install across plugins. OK. | [Acceptable] | N/A |
| 23.10 | I | WaterFlowSensor.h / YFS201Sensor.h / RainSensor.h / WindSensor.h — trivial declarations beyond items above. | [MODULE SAFE for headers beyond 23.x findings] | N/A |

---

