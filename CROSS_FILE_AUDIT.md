# ESP32_Logger — Cross-File Architecture & Linkage Audit

**Scope:** Whole-program call graph / AST review of `src/` + `ESP_Logger.ino` (~19.6k LOC).
**Method:** Per-phase fan-out scan, every claim re-verified with `rg`/`grep` against the entire `src/` tree and `ESP_Logger.ino`.
**Mode:** Diagnostic only — no fixes applied.
**Date:** 2026-05-28

> **Polymorphism note:** `virtual`/`override` methods on `ISensor`, `IModule`, `IExporter`
> and their implementations are *excluded* from dead-code reporting — they are dispatched
> indirectly through registries and base-class pointers, so a missing direct call is expected.

---

## [DEAD CODE]

Each item below is a **non-virtual, non-override** public free function whose only references
are its own declaration (`.h`) and definition (`.cpp`) — i.e. **0 call sites**. All counts
were produced with word-boundary search (`rg -nw '<symbol>' src ESP_Logger.ino`).

| # | Symbol | Declared / Defined | Verification | Result |
|---|--------|--------------------|--------------|--------|
| 1 | `regenerateDeviceId()` | `managers/ConfigManager.h:9` / `.cpp:192` | `rg -nw 'regenerateDeviceId'` | **2 hits = decl+def only — Verified via grep** |
| 2 | `isCaptivePortalDNSRunning()` | `managers/WiFiManager.h:26` / `.cpp:158` | `rg -nw 'isCaptivePortalDNSRunning'` | **2 hits = decl+def only — Verified via grep** |
| 3 | `getRtcTimeString()` | `managers/RtcManager.h:7` / `.cpp:147` | `rg -nw 'getRtcTimeString'` | **2 hits = decl+def only — Verified via grep** |
| 4 | `getStorageBarColor(int)` | `managers/StorageManager.h:9` / `.cpp:90` | `rg -nw 'getStorageBarColor'` | **2 hits = decl+def only — Verified via grep** |
| 5 | `generateDatalogFileOptions()` | `managers/StorageManager.h:10` / `.cpp:112` | `rg -nw 'generateDatalogFileOptions'` | **2 hits = decl+def only — Verified via grep** |
| 6 | `countDatalogFiles()` | `managers/StorageManager.h:11` / `.cpp:148` | `rg -nw 'countDatalogFiles'` | **2 hits = decl+def only — Verified via grep** |
| 7 | `formatFileSize(uint64_t)` | `utils/Utils.h:9` / `.cpp:8` | `rg -nw 'formatFileSize'` | **2 hits = decl+def only — Verified via grep** |
| 8 | `getUsbReservedPins()` | `utils/Utils.h:61` / `.cpp:209` | `rg -nw 'getUsbReservedPins'` | **2 hits = decl+def only — Verified via grep** |
| 9 | `getProfileById(BoardProfileId)` | `core/BoardProfiles.h:85` / `.cpp:147` | `rg -nw 'getProfileById'` | **2 hits = decl+def only — Verified via grep** |
| 10 | `listProfilesCount()` | `core/BoardProfiles.h:93` / `.cpp:163` | `rg -nw 'listProfilesCount'` | **3 hits = decl+def + 1 doc-comment (`h:91`), 0 calls — Verified via grep** |
| 11 | `FlowRunLogger::isRunning() const` | `pipeline/FlowRunLogger.h:55` / `.cpp:209` | `rg -nw 'isRunning'` | **2 hits = decl+def only — Verified via grep** |
| 12 | `FlowRunLogger::runStartEpoch() const` | `pipeline/FlowRunLogger.h:56` / `.cpp:215` | `rg -nw 'runStartEpoch'` | **2 hits = decl+def only — Verified via grep** |

**Notes**
- Items 4–6 (`StorageManager`) and 1–2 look like the residue of an older server-rendered UI
  that was replaced by the JSON-API + static `/www/` model — they format HTML/option strings
  that nothing now consumes.
- Items 11–12 are ordinary (non-virtual) accessors on a concrete class; `FlowRunLogger` is
  driven entirely through `begin()`/`update()` from `StorageTask`, and the run-state accessors
  are never read. Safe to remove or wire into `/api/*`.
- **Not flagged** (intentionally retained): all `ISensor`/`IExporter`/`IModule` overrides,
  and `AlertEngine::begin/load/fromJson` — these *are* referenced (e.g. `alertEngine.begin()`
  in `ESP_Logger.ino`, `fromJson` in `ApiHandlers.cpp`).

---

## [CONTRACT MISMATCH]

### CM-1 — `mkdir()` failure silently swallowed; never reaches `StorageTask` (most actionable)
- **Producer:** `fs::FS::mkdir()` returns `bool`.
- **Consumer:** `storage/CsvLogger.cpp:24` — `if (!_fs->exists(_dir)) _fs->mkdir(_dir);`
  The result is discarded.
- **Cross-boundary contract break:** `CsvLogger::begin()` (`storage/CsvLogger.h:23`) returns
  **`void`**, so even if directory creation fails there is no channel to report it upward.
  In `tasks/StorageTask.cpp:57-60` the caller does `primary.begin(...)` and leaves
  `writingEnabled = true`. On a full/corrupt/RO filesystem every subsequent `appendRow()`
  fails **silently** while the task believes it is logging. Mirror path (`:62`) is identical.
- **Recommendation (diagnostic):** make `_ensureDir()`/`begin()` return `bool` and have
  `StorageTask` flip `writingEnabled` to `false` (and surface a health flag) on failure.

### CM-2 — `limit` query param: signed `long` → `size_t` truncation (defended, but latent)
- **Producer:** `String::toInt()` returns `long`. `web/ApiHandlers.cpp:78`.
- **Consumer:** same line — `size_t limit = (size_t)req->getParam("limit")->value().toInt();`
- **Hazard:** `limit=-100` becomes ~`4.29e9` after the unsigned cast. The guard
  `if (limit < 1)` (`:80`) cannot catch it (a huge value is not `< 1`); only the following
  `if (limit > 300) limit = 300;` (`:81`) rescues it. Correct *today* purely by ordering luck.
- **Recommendation:** parse into a signed `long`, validate `>= 1`, then clamp before casting.

### CM-3 — `IExporter::maxRetries()` (`uint8_t`) compared against `int` loop counter
- **Producer:** `export/IExporter.h:27` — `virtual uint8_t maxRetries()`.
- **Consumer:** `export/ExportManager.cpp:70` — `for (int attempt = 0; attempt <= exp->maxRetries(); attempt++)`.
- **Hazard:** Low. `uint8_t` promotes cleanly to `int` for values 0–255; no truncation here.
  Flagged only as an implicit-conversion contract note — keep types consistent.

### CM-4 — `int` counts → `size_t` parameters across task/exporter boundary
- **Producer:** `tasks/ExportTask.cpp:17` — `int batchCount`; `sensors/SensorManager.cpp:188` — `int n = s->readAll(...)`.
- **Consumer:** `export/ExportManager.h:30` — `sendAll(const SensorReading*, size_t count)`;
  array indexing `readings[j]` with `int j` (`SensorManager.cpp:229`).
- **Hazard:** Low/defended — both are bounded by small compile-time caps
  (`EXPORT_BATCH_SIZE`, `readAll(out, 4)`), and a guard prevents negatives reaching the
  cast. Noted for type-hygiene; not a live defect.

### CM-5 — Sentinel/enum cast on JSON ingest
- **Location:** `export/ExportManager.cpp:170` — `sr.quality = (SensorQuality)(doc["q"] | 0);`
- **Hazard:** Correct by design (missing key → `0` = `QUALITY_UNKNOWN`, `SensorTypes.h:8`),
  but an out-of-range integer in the spool file would cast to an undefined enumerator with no
  validation. Low severity; recommend clamping to `QUALITY_ERROR` for unknown values.

> **Verified clean:** the primary hot path `SensorTask → sensorQueue → ProcessingTask →
> storageQueue/exportQueue` carries `SensorReading` by value with matching `sizeof()` element
> sizes (`TaskManager.cpp:117-119`); no width/sign mismatch on the core data flow.
> `AlertEngine::begin()`/`alertEngine.load()` return values **are** checked at their call sites.

---

## [MEMORY LEAK / LIFECYCLE]

### ML-1 — `/upload` handler: `UploadCtx` early-exit paths never register the disconnect cleaner
- **File:** `web/WebServer.cpp:1826-1938` (multipart upload `onUpload` lambda).
- **Allocation sites (heap, stored in `request->_tempObject`):**
  auth-fail `:1856`, invalid path `:1866`, invalid filename `:1874`, no-FS `:1889`,
  protected-path `:1897`, disk-full `:1910` — each does `new UploadCtx{...}; ... return;`.
- **Cleanup is split across two callbacks:**
  - `onRequest` (`:1830-1836`) `delete ctx` — fires only on *normal* request completion.
  - `onDisconnect` (`:1931-1937`) `delete c` — **registered only on the success path**, after
    all the early `return`s above.
- **Cross-boundary hazard:** if the client aborts the connection after one of the six
  early-exit allocations but before `onRequest` runs, no handler owns the `UploadCtx`. The
  framework's `~AsyncWebServerRequest` then reclaims `_tempObject` with **`free()`** — which
  (a) mismatches the `new` allocation and (b) skips the `File` member's destructor, so an
  open `File` handle can leak on the early-exit-then-abort race. Owner is ambiguous because
  the disconnect cleaner isn't installed before the object can escape scope.
- **Recommendation:** allocate `UploadCtx` once at `index==0` and register `onDisconnect`
  **immediately** (as the OTA handler already does — see ML-OK below), before any validation.

### ML-2 — `/import_settings`: allocation-before-handler window
- **File:** `web/WebServer.cpp:2038-2067`.
- **Detail:** `String* buf = new (std::nothrow) String();` (`:2043`) is stored into
  `req->_tempObject` (`:2052`) *before* `onDisconnect` is registered (`:2058-2061`).
  A disconnect inside that small window leaks the `String` (again reclaimed via `free()`,
  not `delete`). Narrow but real; move the handler registration to right after allocation.

### ML-3 — Hot-path `String`-by-value returns across file boundaries (low / fragmentation)
- **Signatures:** `web/WebServer.h:55-56` `getModeDisplay()` / `getNetworkDisplay()` return
  `String` by value and are invoked from `publishLiveEvent()` (~1 Hz). Also
  `RtcManager.h:7-10`, `StorageManager.h:10/12`, `UsbCdcModule.h:43-65`.
- **Note:** these are *returns* (RVO/move-eligible), not by-value parameters, so the cost is
  modest — but on the 1 Hz SSE path repeated `String` construction adds avoidable heap churn
  on a fragmentation-sensitive ESP32-C3. Consider `const char*`/caller-supplied buffers for
  the live path. No by-value `String` *parameters* cross boundaries (checked — none found).

> **Verified safe (no action needed):**
> - **FreeRTOS queues** pass `SensorReading` **by value**; the struct is pure POD (fixed
>   `char[]`, scalars, no internal pointers — `core/SensorTypes.h:19-31`), so no stack-lifetime
>   pointer ever enters a queue. Queue element size = `sizeof(SensorReading)`.
> - **OTA handler** (`WebServer.cpp:2178-2192`) allocates `OtaCtx` and registers `onDisconnect`
>   *immediately* — the correct pattern ML-1/ML-2 should follow.
> - Scoped `new[]`/`delete[]` and `malloc`/`free` pairs in `ApiHandlers.cpp:248→295`,
>   `HttpExporter.cpp:61→101`, `OpenSenseMapExporter.cpp:49→90`, `AggregationEngine.cpp:213→238`,
>   `ConfigManager.cpp:418→{434,450,478}`, and `unique_ptr<char[]>` in `WebServer.cpp:812-813`.
> - **ArduinoJson:** `as<const char*>()` results are `strncpy`'d into fixed buffers in the same
>   scope (e.g. `HttpExporter.cpp:37`, `AlertEngine.cpp:130`) — no pointer outlives its document.

---

## [HEADER BLOAT]

Heavy headers pulled into `.h` files where the type is **not used in any declaration**
(only in the matching `.cpp`), so the include can move to the `.cpp` or be forward-declared.
Each was confirmed by reading the header's public surface.

| # | Header | Heavy include | Usage in header | Recommendation |
|---|--------|---------------|-----------------|----------------|
| 1 | `web/WebServer.h:5` | `<ArduinoJson.h>` | none — no `Json*` type in any decl; already pulled transitively via `utils/JsonResponse.h` (`:7`) | **Remove** (redundant) |
| 2 | `export/HttpExporter.h:3` | `<HTTPClient.h>` | none — class stores only `char[]`; `HTTPClient` used only in `.cpp` | **Move to `.cpp`** |
| 3 | `export/WebhookExporter.h:3` | `<HTTPClient.h>` | none in decls | **Move to `.cpp`** |
| 4 | `export/OpenSenseMapExporter.h:3` | `<HTTPClient.h>` | none in decls | **Move to `.cpp`** |
| 5 | `export/SensorCommunityExporter.h:3` | `<HTTPClient.h>` | none in decls | **Move to `.cpp`** |
| 6 | `core/Globals.h:6` | `<FS.h>` | only `extern fs::FS* activeFS;` (pointer) | **Forward-declare** `namespace fs { class FS; }` |
| 7 | `core/Globals.h:5,7` | `<LittleFS.h>`, `<SD.h>` | none — only back `bool sdAvailable/littleFsAvailable` flags | **Move to `Globals.cpp`** |
| 8 | `sensors/plugins/ENS160Sensor.h:3` | `<Wire.h>` | none in decls (driver internals only) | **Move to `.cpp`** |
| 9 | `sensors/plugins/BH1750Sensor.h:3` | `<Wire.h>` | none in decls | **Move to `.cpp`** |
| 10 | `sensors/plugins/SCD4xSensor.h:3` | `<Wire.h>` | none in decls | **Move to `.cpp`** |
| 11 | `sensors/plugins/BME280Sensor.h:3` | `<Wire.h>` | none in decls (keep `BME280_Mini.h` — `_bme` is a value member) | **Move `<Wire.h>` to `.cpp`** |
| 12 | `sensors/plugins/VEML6075Sensor.h:3` | `<Wire.h>` | none in decls | **Move to `.cpp`** |
| 13 | `sensors/plugins/VEML7700Sensor.h:3` | `<Wire.h>` | none in decls | **Move to `.cpp`** |
| 14 | `sensors/plugins/SGP30Sensor.h:3` | `<Wire.h>` | none in decls | **Move to `.cpp`** |
| 15 | `tasks/StorageTask.h:4` | `<FS.h>` | `StorageTaskParam` holds only `fs::FS*` pointers (`:25`) | **Forward-declare** `fs::FS` |
| 16 | `managers/StorageManager.h:3` | `<FS.h>` | declarations return `fs::FS*` (pointer) only | **Forward-declare** `fs::FS` |

**Impact:** `<Wire.h>`, `<FS.h>`, `<HTTPClient.h>` and especially `<ArduinoJson.h>` are large
template/transitive headers; the `Wire`/`HTTPClient` moves alone touch ~10 plugin/exporter TUs
and should measurably cut incremental rebuild time.

**Deliberately kept** (full definition genuinely required in the header — *not* bloat):
`IModule.h`, `ISensor.h`, `IExporter.h`, `ModuleRegistry.h`, `AlertEngine.h`,
`SensorManager.h`, `SerialProvisioner.h` (ArduinoJson types appear *by value* in
virtual/inline signatures); `CsvLogger.h`, `FlowRunLogger.h`, `Utils.h`, `AtomicWrite.h`
(use `fs::FS&` by reference in signatures / inline templates); `BME280_Mini.h`,
`BME688_Mini.h`, `MQTT_Mini.h` (`TwoWire*`/`WiFiClient*` used in inline methods);
`MqttExporter.h` (`WiFiClient`/`WiFiClientSecure` value members); `JsonResponse.h`
(inline function bodies need both definitions).

---

## Summary

| Category | Confirmed items | Highest severity |
|----------|-----------------|------------------|
| Dead code | 12 (all grep-verified, 0 call sites) | n/a — cleanup |
| Contract mismatch | 5 (1 actionable: CM-1) | **CM-1** silent storage failure |
| Memory lifecycle | 2 active (ML-1, ML-2) + 1 low (ML-3) | **ML-1** abort-race ctx/File leak |
| Header bloat | 16 movable includes across ~14 files | build-time only |

**Top three to address first:** CM-1 (silent log-write failure on `mkdir`/FS error),
ML-1 (`/upload` disconnect-race leak), and Header-bloat #8–14 (`<Wire.h>` in 7 sensor headers).
