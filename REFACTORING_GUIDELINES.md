# Master Implementation Guideline (MIG)

**Status:** Authoritative. Every refactor commit must conform.
**Scope:** All code in `src/` and `www/`.
**Source:** Synthesis of recurring C/H findings in [AUDIT_LOG.md](./AUDIT_LOG.md).
**Audience:** Any engineer or AI agent executing fixes against the audit findings.

These are **Standard Operating Procedures (SOPs)**, not suggestions. A pull request that violates any rule below must either (a) update this document with rationale + reviewer sign-off, or (b) be rejected.

---

## Architecture map

Source tree layout (see comment block in `ESP_Logger.ino`):

```
src/
├── core/         Config structs, enums, BoardProfiles, IModule interface
├── managers/     ConfigManager, HardwareManager, OtaManager, StorageManager, RtcManager
├── modules/      IModule adapters: WiFi, OTA, Time, Theme, DataLog
├── sensors/      ISensor + SensorManager + plugins/  (19 plugin classes)
├── pipeline/     DataPipeline queues, AggregationEngine (LTTB), LiveAggregator
├── storage/      JsonLogger (JSON Lines), CsvLogger (wide-CSV)
├── export/       IExporter, MQTT / HTTP / Webhook / SC / OSM exporters
├── tasks/        TaskManager, 4 FreeRTOS tasks (Sensor, Process, Storage, Export)
├── alerts/       AlertEngine + toast ring buffer
├── drivers/      Internal mini drivers (BME280, BME688, DS18B20, MQTT, DS1302)
├── utils/        MutexGuard, AtomicWrite, IsrPin, JsonResponse
└── web/          WebServer, ApiHandlers, RequireAuth, FirstRunHandler
www/
├── js/           core.js, iot-extensions.js, sensors.js, settings.js, pages.js
└── pages/        Per-settings HTML fragments
```

---

## Shipped helpers

Each helper below MUST be used at every qualifying call site; do not
reimplement inline.

| Helper | File | Contract |
|---|---|---|
| `MutexGuard` | `src/utils/MutexGuard.h` | RAII acquire/release of a `SemaphoreHandle_t` with bounded timeout; caller MUST check `isLocked()` |
| `AtomicWrite` | `src/utils/AtomicWrite.h` | Write-to-tmp → rename pattern for LittleFS; reverts on failure |
| `IsrPin` | `src/utils/IsrPin.h` | RAII wrapper around `gpio_isr_handler_add/remove`; MUST be the last member of any ISR-owning class |
| `RequireAuth` | `src/web/RequireAuth.h` | Rate-limit + CSRF check for mutating handlers; first statement must be `if (!requireMutatingAuth(req)) return;` |
| `JsonResponse` | `src/utils/JsonResponse.h` | Serialize `JsonDocument` to `AsyncResponseStream` and send; use for all standard JSON responses |
| `BoardProfiles` | `src/core/BoardProfiles.h` | Pin-restriction registry; `isPinAllowed()` and `validateAttachPin()` are the gate for all GPIO assignments |
| `fetchWithTimeout` | `www/js/core.js:29` | Fetch with AbortController timeout (default 15 s); use on every API call |
| `getCsrfToken` | `www/js/core.js:449` | Returns a cached promise for the current CSRF token; required before every POST |
| `escapeHtml` / `esc` | `www/js/core.js:799` | HTML-escape for `innerHTML` interpolation; required on every user-controlled string |

---

## Module lifecycle (R20)

`IModule` (`src/core/IModule.h`) defines the contract for all runtime
subsystems with persisted config:

- `load(cfg)` — merge JSON into in-memory state; called at boot and on
  `POST /api/modules/:id`.
- `save(cfg)` — serialise state to JSON for persistence.
- `start()` — bring the module online with current config. Returns `false`
  if a device reboot is required to apply the change (e.g. WiFiModule,
  which cannot tear down the radio from the AsyncTCP worker thread).
- `stop()` — release resources.

`POST /api/modules/:id/restart` calls `stop()` then `start()` without
changing the enabled flag (`src/web/ApiHandlers.cpp:657`). If `start()`
returns `false`, the response includes `"restartRequired":true` and the
caller must POST `/restart`.

Modules that currently override `start()`:
- `WiFiModule::start()` — always returns `false` (reboot required)
  (`src/modules/WiFiModule.h:26`)
- `TimeModule::start()` — queues an NTP sync task, returns `true`
  (`src/modules/TimeModule.h:23`)

`POST /api/modules/:id/enable?on=0` sets the enabled flag only; a subsequent
`/restart` or `/api/modules/:id/restart` is needed for most modules to act on it.

---

## Concurrency patterns (R14)

### SensorManager reload

`SensorManager::tickFiltered` acquires `configMutex` before iterating
`_sensors[]`. `reloadConfig` acquires the same mutex in write-mode. This
prevents use-after-free when a reload deletes a plugin while the sensor task
is iterating (`AUDIT 3.19`, fixed in R14, `src/pipeline/DataPipeline.h:22`).

### AlertEngine MQTT publish

`AlertEngine::evaluate()` stages MQTT publish payloads into `_pendingMqtt[]`
while holding `_mutex`, then releases the mutex and drains the array outside
it. This prevents holding `_mutex` during a blocking MQTT network call
(`src/alerts/AlertEngine.h:112-114`).

### RingBuffer ordering

Writers hold `webDataMutex` for the push. Readers hold the same mutex.
A producer that cannot acquire the mutex within 5 ms increments
`g_ringPushDrops` and drops the reading rather than blocking the sensor task
(`AUDIT 2.8`, `src/pipeline/DataPipeline.h`).

---

## Pin assignment (R11 + R17)

- `PIN_UNSET` (0xFF) is the sentinel for "no pin chosen" in every
  `uint8_t` GPIO field (`src/core/BoardProfiles.h:28`).
- Every sensor plugin's `init()` MUST call `validateAttachPin(pin, sensorId,
  fieldName)` before `pinMode` or `gpio_isr_handler_add`. On failure, log
  the reject reason and return `false`.
- `isPinAllowed(profile, pin, purpose)` returns `false` for `PIN_UNSET`,
  strap, USB, flash, and reserved pins (`src/core/BoardProfiles.h`).
- Serial1 ownership: `_claimSerial1(who)` / `_releaseSerial1(who)` in
  `src/sensors/SensorManager.cpp:14` prevent two UART sensors from binding
  the same hardware serial port.
- I2C address ownership: `_claimI2cAddress(addr, who)` /
  `_releaseI2cClaims(who)` in `src/sensors/SensorManager.cpp:29` prevent
  two sensors from sharing a fixed I2C address.

---

## Validation contracts

- `requireMutatingAuth(req)` (`src/web/RequireAuth.h`) — FIRST statement in
  every mutating HTTP handler; performs rate-limit check then CSRF check.
- Numeric clamps and `isPinAllowed` — R10 (`PR #85`) applied min/max guards
  to all numeric config fields at the `/save_*` boundary.
- JSON escape — `toJsonLine` and all exporter `send()` methods MUST route
  user-controlled strings through the JSON escape helper (Pillar 2.7).
- `isPathProtected(path)` — returns `true` for config, spool, and temp files;
  MUST be consulted by `/download`, `/delete`, and `/move_file`
  (Pillar 2.4, `src/web/WebServer.cpp`).
- HTML escape — every `innerHTML` interpolation MUST use `esc()` from
  `www/js/core.js:799` (Pillar 2.8).

---

## Frontend conventions

- `fetchWithTimeout(url, opts, timeoutMs)` (`www/js/core.js:29`) — use on
  every API call. Default 15 s; 30 s for writes; 60 s for imports/exports.
- `getCsrfToken()` (`www/js/core.js:449`) — required before every mutating
  POST. Passes the token as a `?csrf=` query parameter.
- `esc(s)` (`www/js/core.js:799`) — required on every `innerHTML`
  interpolation of server-supplied data.
- CDN script loading — `<script src>` from CDN MUST include
  `integrity="sha384-..."` and `crossorigin="anonymous"`. Version MUST be
  pinned (not a range). uPlot is pinned to `1.6.32` with SRI hashes in
  `www/js/pages.js:42-43` (R18).

---

## Deferred / out of scope

The following items were identified in the audit but not yet resolved:

- Bundled CA store for HTTPS exporters — `setInsecure()` is used until a CA
  store ships (audit 15.2 / REFACTORING_GUIDELINES Pillar 2.9).
- `WiFi.persistent` / NVS divergence — AP credentials written to NVS by the
  Arduino SDK can diverge from `config.bin` after a factory reset (audit 14.7).
- Test scaffold — no automated test harness exists; CI is build-only.
- SPA accessibility full pass — only the failsafe page and firstrun wizard
  have received WCAG 2.2 attention (R18); the rest of the SPA is untouched.

---

## Pillar 1 — Concurrency & FreeRTOS

### 1.1 Lock-ordering invariant

When two or more mutexes are held simultaneously, they MUST be acquired in this fixed order:

```
configMutex  →  fsMutex  →  wireMutex  →  rtcMutex  →  webDataMutex  →  class-local _mutex
```

- Reverse-order acquisition is FORBIDDEN.
- Skipping levels is allowed (a holder of `fsMutex` may acquire `_mutex` without going through `wireMutex`/`webDataMutex`).
- Releasing is in the strict reverse order of acquisition.
- Any new mutex MUST be assigned a position in this order before merging.

### 1.2 Mutex acquisition rules

- **No `portMAX_DELAY` in production paths.** Every `xSemaphoreTake` MUST specify a bounded timeout (recommendations: hot path 50 ms, normal 500 ms, batch 2000 ms; **MAX 3000 ms**).
- On timeout, the caller MUST fail-safe: return `false` / 503 / `BUSY` to the caller. **NEVER proceed-without-lock.** (Eliminates the "saveConfig proceeds even on timeout" class — AUDIT 1.8.)
- Mutex acquire/release MUST use RAII via a single `MutexGuard` class in `src/utils/MutexGuard.h`. Manual `xSemaphoreGive` calls are FORBIDDEN except inside `MutexGuard`'s implementation.
- `xSemaphoreGive` MUST never be called without verifying the corresponding take succeeded. (Eliminates AUDIT 3.13 factory_reset assert.)

### 1.3 fsMutex universal contract

- **Every LittleFS / SD write call site MUST acquire `fsMutex`.** No exceptions. This closes the ~12 "mutex-bypass" sites flagged in FA.1.
- Read-only file opens MAY skip `fsMutex` IF the file is documented immutable-during-runtime (e.g. `/www/*.html`, `/uPlot.iife.min.js`).
- Acquisition timeout: **2000 ms**. Failure path: return `false` and increment `g_fsMutexTimeouts`.
- File operations holding `fsMutex` MUST complete in < 500 ms or release-and-reacquire between sub-operations.

### 1.4 webDataMutex symmetry

- Producers (ProcessingTask) MUST use the same timeout class as readers (web handlers). Try-take-0 in the producer is FORBIDDEN (AUDIT 2.8 / FA.4).
- Recommended: 5 ms timeout in producer hot path; on miss, increment `g_ringPushDrops` for observability.

### 1.5 configMutex scope rules

- `configMutex` covers ALL of `DeviceConfig` AND all sensor plugin objects accessible via `SensorManager`.
- `SensorManager::tickFiltered` MUST acquire `configMutex` in read-mode (or use a shared/exclusive variant) before iterating `_sensors[]`. (Eliminates AUDIT 3.19 / 15.3 use-after-free on reload.)
- `reloadConfig` MUST acquire in write-mode; if any reader holds it, wait or 503.

### 1.6 Blocking I/O contract

Tasks MUST NOT hold any mutex during blocking I/O:

- TLS / HTTPS network calls
- MQTT publish (until proven non-blocking)
- NTP sync
- Sensor reads exceeding 10 ms
- LittleFS operations exceeding 500 ms

If blocking I/O is necessary, the calling task must: (a) snapshot state under mutex into stack locals, (b) release mutex, (c) perform the blocking call, (d) re-acquire to commit the result.

### 1.7 Heartbeat refresh contract

- Any task that loops > 1 s without yielding to its scheduler MUST refresh its slot: `g_taskHeartbeat[TASK_IDX_X] = millis();`
- Drain loops (StorageTask inner queue drain, ExportTask retry loops) MUST cap iterations or include intermediate heartbeat refresh.
- C4 watchdog threshold is 30 s — design for headroom of 5×.

### 1.8 Task priority assignment

- Producers > consumers (existing).
- `TASK_PRIO_STORAGE` MUST be raised from 1 → 2 to match `TASK_PRIO_PROCESS`; otherwise ExportTask at the same priority blocks StorageTask cooperatively (FA.2 priority inversion).
- `TASK_PRIO_EXPORT` stays at 1 — exports are inherently best-effort.

### 1.9 Transactional init/shutdown

- `TaskManager::init()` MUST be all-or-nothing. On any sub-step failure, every prior allocation MUST be unwound before returning false.
- Callers MUST check the return: `if (!TaskManager::init(fs)) { /* abort, do not start web */ }`. (Closes AUDIT 2.1.)
- `running = true` MUST be set ONLY after every queue/mutex/task has been successfully created.
- `shutdown()` MUST wait for `eTaskGetState(handle) == eDeleted` for each task (not just `vTaskDelay`).

---

## Pillar 2 — Security & API Contract

### 2.1 Universal mutating-handler header

The FIRST statements in EVERY mutating HTTP handler MUST be:

```cpp
if (!requireMutatingAuth(req)) return;
```

- `requireMutatingAuth` is the SINGLE authorized helper that performs (in order): rate-limit check → CSRF token validation → optional auth check.
- No handler may call `rateLimit429` or `csrfBlock` directly.
- A new mutating endpoint that doesn't call this first statement is a release-blocker.

### 2.2 Endpoints that MUST be guarded

The following endpoints currently lack one or both protections and MUST be patched:
`/factory_reset`, `/restart`, `/do_update`, `/backup_bootcount`, `/restore_bootcount`, `/rtc_protect`, `/flush_logs`, `/sync_time`, `/wifi_scan_start`, `/wifi_scan_result`, `/api/regen-id`, `/api/alerts`, `/api/alerts/snooze`, `/api/modules/:id`, `/api/modules/wifi/test`.

### 2.3 CSRF token acceptance

- `csrf` query/form param continues to work (current pattern).
- `X-CSRF-Token` HTTP header MUST also be accepted (for JSON-body endpoints where the param parser cannot extract from the body).
- Token comparison MUST be constant-time (current `secureEq` is correct).
- Token MUST rotate on every successful confirm of a sensitive operation (factory_reset, OTA confirm). Per-boot-only scope (AUDIT 7.6) is insufficient.

### 2.4 Protected path list

`isPathProtected(path)` MUST return true for ALL of:
- `/config.bin`, `/bootcount.bin`, `/reset_log.txt`, `/config.tmp`, `/bootcount.tmp`
- `/alerts.json`, `/alerts.json.tmp`
- `/platform_config.json`, `/platform_config.json.tmp`
- Any path starting with `/config/`
- Any path starting with `/spool/`
- Any path starting with `/_setup/`

`/download`, `/delete`, `/move_file`, `/upload` MUST consult `isPathProtected` for both source and destination and 403 on match. (Closes AUDIT 3.6 / 7.1.)

### 2.5 Credential exposure

- `/export_settings`, `/api/backup`, and any GET that includes secrets MUST mask the following fields by default: `apPassword`, `clientPassword`, `password` (MQTT), `access_token` (OSM), `Authorization` (HTTP exporter).
- Default mask value: `"***"`.
- Caller MUST pass `?include_secrets=1` AND a fresh CSRF token to unmask. The unmask path MUST log the access via Serial + `/reset_log.txt`.
- Frontend MUST display secrets via a deliberate "Show" toggle that triggers the unmask fetch — never via auto-populated `<input type="password" value="...">`.

### 2.6 String validation at API boundary

User-supplied strings MUST pass through a validator at the boundary they enter the system:

| Field | Validator |
|---|---|
| `sensorId`, `metric`, `sensorType` | `^[a-zA-Z0-9._-]{1,16}$` |
| `clientSSID`, `apSSID` | UTF-8 printable, len ≤ 32 |
| `clientPassword`, `apPassword` | len 0 or 8-64, no `\r\n\0` |
| `ntpServer`, `mqtt.broker`, `http.url` | RFC-1123 host or full URL |
| Color fields (`primaryColor`, etc.) | `^#[0-9a-fA-F]{6}$` |
| File paths (`logoSource`, `faviconPath`, `chartLocalPath`) | `^/[a-zA-Z0-9._/-]+$`, no `..`, no `//` |
| HTTP header values | strip `\r\n\0` before storing |
| IPv4 (`apIP`, `gateway`, ...) | each octet 0-255 via dedicated `parseIPv4` helper |

Validation failure MUST reject the entire save (return false / 400), not silently drop the field.

### 2.7 JSON escape contract

- Any function that emits a JSON string field built from user-controlled data MUST escape via `jsonEscape(out, dst, dstLen)` before snprintf.
- `SensorReading::toJsonLine` MUST be updated to escape `sensorId`/`sensorType`/`metric`/`unit`.
- Every exporter that builds JSON bodies (`HttpExporter::send`, `WebhookExporter::_fireRule`, `OpenSenseMapExporter::send`) MUST route strings through the same helper.

### 2.8 HTML escape contract (frontend)

- Every `innerHTML` site that interpolates server data MUST wrap each value in `esc()`.
- For option lists / dropdowns, prefer `opt.textContent = value; opt.value = value` (DOM-native escape) — see `settings.js:1068-1073` as the model.
- The Handlers map (`core.js:67`) and null-prototype `Object.create(null)` pattern MUST remain. Any new event-dispatcher additions MUST go through `registerHandlers({...})` — no direct `window[name]` lookups.

### 2.9 TLS / HTTPS posture

- Every `HTTPClient.begin(url)` site MUST either (a) call `client.setCACert(...)` with a pinned cert, or (b) explicitly call `client.setInsecure()` AND the firmware MUST have been built with `-D ALLOW_INSECURE_HTTPS=1`.
- `WiFiClient` to MQTT broker MUST add an opt-in `WiFiClientSecure` variant for cloud deployments.
- All exporters MUST surface a "TLS" config option distinct from "Enabled". Default: TLS=on for any URL with `https://` scheme; refuse to send if cert verification can't be set up.

### 2.10 CDN script loading

- Any `<script src=>` loaded from a third-party CDN (e.g. `cdn.jsdelivr.net`) MUST include both `integrity="sha384-..."` and `crossorigin="anonymous"` attributes.
- Version MUST be pinned (no `uplot@1` wildcards).
- `chartLocalPath` and any other user-set `<script src>` target MUST pass validator 2.6's file-path rule. Protocol-relative URLs (`//host/...`) are FORBIDDEN.

---

## Pillar 3 — Memory & Resilience

### 3.1 Heap allocation

- `new T[N]` and `new T()` in any code path reachable from a web handler, sensor read, or task loop MUST use `std::nothrow`.
- Null check MUST follow every `new`. On null: return `false` / 500 + `{"ok":false,"error":"out_of_memory"}`.
- `alloca()` is FORBIDDEN. Use a class member, BSS static, or `std::nothrow` heap allocation.
- Large transient buffers (≥ 1 KB) MUST be heap or member-field, never stack-local. Task stack budgets are listed in `setup.h` and MUST NOT be exceeded.

### 3.2 Atomic file write

ALL writes to LittleFS that REPLACE an existing file (config, modules.json, alerts.json, bootcount.bin, csv rotation, etc.) MUST use the shared helper:

```cpp
bool atomicWrite(fs::FS& fs, const char* path,
                 std::function<bool(File&)> writerFn);
```

Implementation contract:
1. Acquire `fsMutex` (2 s timeout).
2. Open `${path}.tmp` `FILE_WRITE`.
3. Invoke `writerFn(tmpFile)`; on false return → close, remove tmp, return false.
4. Verify `serializeJson` (or whatever writer) returned > 0 AND matches `measureJson(doc)` for JSON writers (closes AUDIT 6.14).
5. `tmpFile.close()`.
6. `fs.rename(tmpPath, path)` — LittleFS rename overwrites atomically.
7. On any step failure: `fs.remove(tmpPath)`, release mutex, return false.

`remove(target); rename(tmp, target);` two-step is FORBIDDEN — leaves both files gone on power-loss (AUDIT 9.8 / 12.11 / 16.1).

### 3.3 LittleFS mount policy

- `LittleFS.begin(true, ...)` (`formatOnFail=true`) is FORBIDDEN in production builds.
- Set `formatOnFail=false` and surface mount failure to the user via the failsafe UI.
- A separate `/factory_reset` endpoint is the ONLY path that may call `LittleFS.format()`, and it MUST require fresh CSRF + a user-typed confirmation token in the POST body (e.g. `confirm=ERASE`).

### 3.4 File-size guard

- Every JSON file read MUST go through `loadJsonFile(fs, path, doc, maxBytes = 16384)`. Reject parse if `f.size() > maxBytes`.
- `/upload` MUST check `LittleFS.totalBytes() - LittleFS.usedBytes() >= contentLength + 32 KB headroom` before accepting bytes (current check at WebServer.cpp:1593 is good — keep it).

### 3.5 OOM / disk-full handling

- Drain loops that consume from a queue MUST cap iteration count (e.g. 64 items) and refresh heartbeat between batches. (Closes AUDIT 11.6.)
- Retry chains with exponential backoff MUST cap total wait at 15 s × heartbeat refresh. (Closes AUDIT 11.8 / 18.1.)
- Spool file writes MUST check `_spoolFS->usedBytes() vs totalBytes()` before append. On disk-full: drop the spool batch, increment `g_spoolDrops`, do not silently corrupt.
- Every error path MUST `return false` or emit an error status. Silent failure (`return true` after a failed operation) is FORBIDDEN.

### 3.6 String / heap discipline

- Arduino `String` is FORBIDDEN as a member of any `extern` global declared in `Globals.h`. Replace with `char[N]` arrays. (Closes AUDIT 6.3 cross-task UAF.)
- `String` is FORBIDDEN in any function called from an ISR or from the IRAM-resident path.
- Within tasks/handlers: short-lived `String` is acceptable for stack-local use; multi-fragment concat that allocates > 64 B SHOULD use `snprintf` into a stack buffer.
- `getRtcTimeString()` and similar `String`-returning hot-path helpers MUST be replaced with `void getRtcTimeStr(char* out, size_t n);`.

### 3.7 Restart circuit breaker

- A monotonic restart counter MUST live in RTC slow memory.
- On boot, if `consecutive_resets_within_60s >= 3`, enter SAFE_MODE: skip `_initPlatform`, skip sensor pipeline, start AP-only web server with the failsafe UI.
- Manual `/restart` and `/factory_reset` MUST zero the counter.
- This is the single mitigation for the FC.4 brick scenario; it is mandatory.

### 3.8 OTA confirmation policy

- `OtaManager::confirm()` MUST NOT be called from the `shouldRestart` path nor `_doSleep`. (Closes FC.1.)
- The 90 s `tick()` deadline is the ONLY auto-confirmation path. A buggy image that crashes in < 90 s must never get confirmed.
- Manual `/api/ota/confirm` requires CSRF + a user prompt.

---

## Pillar 4 — Hardware & ISR Safety

### 4.1 GPIO defaults

R11 replaced the `src/hardware/PinMap_<target>.h` proposal with
`src/core/BoardProfiles.h`. All new pins default to `PIN_UNSET` (0xFF); the
first-run wizard collects GPIO assignments validated against the chosen
profile. Forbidden pin sets per profile are defined in
`src/core/BoardProfiles.cpp` (strap, USB, flash, reserved lists).

`DefaultPins` namespace in `src/core/Config.h` initialises every hardware
config field to `PIN_UNSET`. Devices with a saved `config.bin` keep their
stored values; new devices run the wizard before any sensor starts.

### 4.2 Pin validator

The R11 implementation (`src/core/BoardProfiles.h`) provides:
- `isPinAllowed(profile, pin, purpose)` — returns `false` for `PIN_UNSET`,
  strap, USB, flash, and reserved pins.
- `validateAttachPin(pin, sensorId, fieldName)` — runtime guard called by
  every plugin `init()`; logs a descriptive rejection reason and returns
  `false` on invalid pins.

These replace the proposed `validatePin(int, PinUsage)` and are already
wired into `SensorManager` and the first-run wizard. The `PinPurpose` enum
(`src/core/BoardProfiles.h:26`) covers generic, digital in/out, analog,
I2C, UART, and pulse roles.

### 4.3 Input pull resistors

- Every `pinMode(pin, INPUT)` MUST be replaced with `pinMode(pin, INPUT_PULLUP)` or `INPUT_PULLDOWN`.
- Sensor plugins that interpret floating inputs as "no signal" are FORBIDDEN; the plugin MUST set a defined pull state in `init()`.
- 5 V-only modules (HC-SR04) MUST document the required level shifter; alternatively the plugin MUST refuse to initialise without a build flag confirming the hardware.

### 4.4 ISR atomicity

- ISR functions MUST be `IRAM_ATTR`.
- ISR-touched data MUST be `volatile` AND `uint32_t`-aligned (single-word atomic on RV32 reads/writes).
- ISR read-modify-write MUST use `__atomic_fetch_add` (or `std::atomic<uint32_t>::fetch_add` with `memory_order_relaxed`). Naked `++` is FORBIDDEN in ISR context. (Closes AUDIT 2.13.)
- Strings (Arduino `String`), `JsonDocument`, `Serial.print*`, FS calls, and any function not marked `IRAM_ATTR` are FORBIDDEN inside ISRs.

### 4.5 ISR ↔ Task bridging

The ONLY allowed ISR-to-task communication patterns:

1. **Atomic counter** — ISR `__atomic_fetch_add(&counter, 1)`; task reads under `noInterrupts()/interrupts()` AND clears with `__atomic_exchange`.
2. **Volatile flag** — ISR sets `volatile bool flag = true`; task reads + clears (write 1 → read → write 0).
3. **FreeRTOS queue with ISR-safe send** — `xQueueSendFromISR(handle, &item, &xHigherPriorityTaskWoken)`. The ISR MUST end with `portYIELD_FROM_ISR(xHigherPriorityTaskWoken);`.

Direct invocation of task functions or shared-non-atomic data from ISR is FORBIDDEN.

### 4.6 ISR lifecycle (gpio_isr_handler_add)

Every plugin that installs an ISR via `gpio_isr_handler_add(pin, _isr, this)` MUST:

1. Wrap the install + uninstall in an `IsrPin` RAII helper (new file `src/utils/IsrPin.h`).
2. Provide a destructor that calls `gpio_isr_handler_remove((gpio_num_t)pin);`.
3. The `IsrPin` member MUST be the LAST member of the plugin class so its destructor runs FIRST during `delete`.

This closes AUDIT 2.6 / 23.2 / 23.3 dangling-`this` use-after-free.

### 4.7 Bit-banged protocol timing

- 1-Wire (`DS18B20_Mini::_readBit`/`_writeBit`) and DS1302 ThreeWire bit operations MUST wrap each bit window in `portDISABLE_INTERRUPTS()` / `portENABLE_INTERRUPTS()` (RV32 macros).
- Each protected window MUST be ≤ 100 µs to avoid starving FreeRTOS scheduler and WiFi ISRs.
- Multi-byte burst reads SHOULD release interrupts between bytes to avoid > 1 ms cumulative disable.

### 4.8 RTC access serialization

- `Rtc->GetDateTime()` and `Rtc->SetDateTime()` MUST acquire `rtcMutex` (new mutex, lock order: between `wireMutex` and `webDataMutex`).
- All call sites (main loop, SensorTask, SlowSensorTask, StorageTask, AlertEngine via `_dispatch`, every API handler) MUST go through a helper that takes the mutex.

### 4.9 I2C bus centralization

- `Wire.begin(sda, scl)` MUST be called EXACTLY ONCE at boot, from `HardwareManager::initI2C()`.
- Sensor plugins MUST NOT call `Wire.begin` in their `init()`. (Closes AUDIT 20.1 bus-reconfig race.)
- Plugins requesting different SDA/SCL pairs MUST fail registration with an error log; only one bus configuration per boot is supported.
- I2C sensors with FIXED addresses (VEML6075/VEML7700 both at 0x10) MUST refuse to coexist; `SensorManager::loadAndInit` MUST detect duplicate-fixed-address pairs and surface via `/api/sensors` status.

### 4.10 Plugin blocking declaration

Any sensor plugin whose `readAll()` may block ≥ 10 ms MUST override:

```cpp
bool isBlocking() const override { return true; }
```

Audited offenders that MUST be fixed: `HCSR04Sensor` (24.1), `ZMPT101BSensor` (24.6), `ZMCT103CSensor` (24.6), `BME688Sensor` (17.10 — already 1 s blocking but missing override). The override routes them to `SlowSensorTask` instead of starving the fast `SensorTask`.

### 4.11 Strap-pin safety net

`HardwareManager::initHardware` MUST call `validateAllConfiguredPins()` at boot:
- Iterates `config.hardware.*` pin fields and every enabled sensor's pin field.
- For each, calls `validatePin(pin, usage)`.
- On any failure: log to Serial + `statusMessage`; REFUSE to enable the offending subsystem (sensor / wake pin / SD pin); continue boot with reduced functionality.

A bricked-by-strap-pin scenario (RainSensor on GPIO 9 — AUDIT 23.1 / 31.1) becomes a logged + degraded boot, not a soft-brick.

---

## Compliance & Enforcement

### Pre-merge checklist

Every refactoring PR MUST tick the following:

- [ ] No new `xSemaphoreTake(_, portMAX_DELAY)` calls
- [ ] No new mutex acquire-and-proceed-on-failure patterns
- [ ] No `new T[N]` without `std::nothrow`
- [ ] No `LittleFS.format()`, no `formatOnFail=true`
- [ ] No raw `fs.open(_, FILE_WRITE)` for replace-existing — go through `atomicWrite()`
- [ ] No mutating HTTP handler without `requireMutatingAuth(req)` as the first statement
- [ ] No `innerHTML +=` with un-`esc()`'d user data
- [ ] No `<script src>` without integrity hash (or local-only path passing validator)
- [ ] No `pinMode(_, INPUT)` without an explicit pull
- [ ] No ISR `++` on shared state (use atomic ops)
- [ ] No new globals in `Globals.h` without lock-order doc + access pattern note
- [ ] AUDIT_LOG.md cross-references: every closed finding has its row updated to `Status = Fixed (commit-hash)`

### Linting & build flags

- `-Wall -Wextra -Werror=return-type -Werror=missing-field-initializers` SHOULD be the build baseline.
- A new CI job MUST grep for forbidden patterns (`portMAX_DELAY` outside the MutexGuard implementation, `new ` without `nothrow` in audited files, `Wire.begin(` outside `HardwareManager.cpp`).

### Deviation procedure

When an SOP cannot be followed (e.g. third-party library forces `portMAX_DELAY`):
1. Open an issue describing the deviation.
2. Add a `// SOP-EXEMPT(reason)` comment at the violating site.
3. Update this MIG with the exemption rationale.
4. Get reviewer sign-off in the PR.

---

[GUIDELINE ESTABLISHED: AWAITING REFACTORING COMMANDS]
