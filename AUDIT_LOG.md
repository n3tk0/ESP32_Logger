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
| 1.10 | M | RtcManager.cpp:27-28 — `delete rtcWire` before `delete Rtc`. Rtc dtor may dereference dangling _wire reference. | Swap order: `delete Rtc; rtcWire = nullptr; delete rtcWire;` | Pending |
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
| 5.3 | C | setup.h:246-254 — default `WEB_BASIC_AUTH_USER "admin"` / `WEB_BASIC_AUTH_PASS "admin"` shipped when `WEB_BASIC_AUTH_ENABLED=1`. | Add `#if WEB_BASIC_AUTH_ENABLED && (strcmp(WEB_BASIC_AUTH_USER,"admin")==0 && strcmp(WEB_BASIC_AUTH_PASS,"admin")==0)\n#error "Override default basic-auth creds before enabling"\n#endif` | Pending |
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
| 6.3 | H | Globals.h:51-52 — `extern String wakeUpButtonStr` / `cycleStartedBy` mutated from loop() AND read by AsyncTCP worker (WebServer.cpp:117). String buffer pointer triple is not atomic → UAF read. | Replace with `char wakeUpButtonStr[16]` + `char cycleStartedBy[16]`; or guard with webDataMutex on every access. | Pending |
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
| 6.14 | H | ModuleRegistry.cpp:108 — `serializeJson` short-write check only catches `n == 0`. Truncated-but-nonzero output passes through rename. | Compare against `measureJson(doc)`; abort + remove tmp if shorter. | Pending |
| 6.15 | M | ModuleRegistry.cpp:36-44 + 84-127 — saveAll does not take fsMutex; loadAll's crash-recovery rename also unlocked. | Acquire `fsMutex` at function entry of saveAll; expose `MutexGuard` helper for symmetry. | Pending |
| 6.16 | L | ModuleRegistry.cpp:116-120 comment promises atomic rename on LittleFS but the class accepts any `fs::FS&`. SD/FAT rename-over-existing fails. | Add probe: if `fs.exists(path)` → `fs.remove(path)` first when target FS != LittleFS. | Pending |
| 6.17 | M | ModuleRegistry.cpp:55-60 — Oversize-file rejection returns false but doesn't quarantine. File stays oversize across reboots. | Same fix as 6.13. | Pending |

---

