# Architectural & UI Audit — ESP32 Water Logger

**Date:** 2026-04-17
**Scope:** Entire repository **except** `full_logger.ino`.
**Branch:** `claude/audit-architecture-ui-EaC4j`
**Goal:** Unify and modernize Web UI/UX and the configuration system without rewriting the project. Incremental, production-safe improvements.

## How to read this report
- Seven passes, each stands alone.
- Passes 1–3 are findings. Passes 4–6 are plans. Pass 7 is a bug list.
- File/line references are intentionally dense — every claim is verifiable.
- Nothing here has been changed yet; implementation starts after the report.

---

## Pass 1 — UI/Config Mapping

### Frontend assets (served from LittleFS `/www/`)
| File | Lines / Size | Role |
|------|--------------|------|
| `www/index.html` | 1949 L / 89 KB | SPA shell + all 14 pages inlined |
| `www/web.js` | 3578 L / 113 KB | All client logic, polling, rendering, 288 functions |
| `www/style.css` | 889 L / 14.5 KB | Single stylesheet, CSS variables for theme |
| `www/chart.min.js` | 19 L / 138 KB | Local fallback Chart.js |
| `www/platform_config.json` | 9 KB | Dynamic sensor descriptor |
| `www/changelog.txt` | 2.4 KB | Served via `/api/changelog` |
| `www/favicon.ico` | 15 KB | Asset |
| `www/xiao_esp32_c3_board.jpg` | 67 KB | Diagram |

### Backend HTTP layer (`src/web/`)
- **`WebServer.cpp`** (1355 L) — `AsyncWebServer` setup, all routes, failsafe/restart HTML inlined in PROGMEM.
- **`WebServer.h`** (46 L) — `setupWebServer()` + helpers.
- **`ApiHandlers.cpp/.h`** (427 L + 8 L) — auxiliary API endpoints.

### UI-serving strategy
- **Normal**: `server.serveStatic("/", LittleFS, "/www/")` when `/www/index.html` exists.
- **Failsafe**: embedded `FAILSAFE_HTML` (~9 KB PROGMEM) on `/` and `/setup`.
- **SPA aliases**: `/dashboard`, `/files`, `/live`, `/settings*` (×11) → 302 redirect to `/`.
- **Restart UX**: PROGMEM `RESTART_HEAD` + `RESTART_TAIL` concatenated at runtime.

### Routes (summary)
- **UI**: `GET /`, `/setup`, SPA redirects.
- **Status/data**: `/api/status`, `/api/live` (500 ms poll), `/api/recent_logs` (3 s), `/api/filelist`, `/api/changelog`, `/api/platform_config`, `/api/platform_reload`.
- **Save**: `/save_device`, `/save_flowmeter`, `/save_hardware`, `/save_theme`, `/save_datalog`, `/save_network`, `/save_time`, `/set_time`, `/save_platform`.
- **FS**: `/upload`, `/download`, `/delete` (GET+POST), `/mkdir`, `/move_file`, `/import_settings`, `/export_settings`.
- **System**: `/restart`, `/factory_reset`, `/flush_logs`, `/backup_bootcount`, `/restore_bootcount`, `/regen-id`, `/rtc_protect`, `/sync_time`, `/wifi_scan_start`, `/wifi_scan_result`, `/do_update` (OTA).

### Transport
- 100% polling. No `AsyncEventSource`, no `AsyncWebSocket`.
- No gzip, no `Cache-Control`, no ETag.

### Config surface (`src/core/Config.h`, 237 L)
- Monolithic packed struct `DeviceConfig { theme, datalog, flowMeter, hardware, network, … }`.
- `ConfigManager.cpp` (510 L): binary `/config.bin` with `CONFIG_STRUCT_MAGIC 0xC0FFEE36`, `CONFIG_VERSION = 12`.
- `platform_config.json` = parallel dynamic descriptor (sensors), disconnected from `DeviceConfig`.

### Key coupling at a glance
- HTML + JS fully inlined together; 14 `#page-*` blocks in one `index.html`.
- Every `save_*` endpoint hand-written per section.
- Theme (13 fields) embedded in `/api/status` payload — resent on every poll.
- Failsafe page duplicates Setup + Core Logic tabs with its own inline CSS + JS (~9 KB PROGMEM).

---

## Pass 2 — UI/UX Audit

### Performance
- **Cold-load payload** = `index.html` 89 KB + `web.js` 113 KB + `style.css` 14.5 KB ≈ **220 KB** uncompressed, plus `chart.min.js` 138 KB on pages that need it.
- **No gzip**: AsyncWebServer supports serving `.gz` with `Content-Encoding: gzip` — unused. Expected win ≈ 60–70 %.
- **No `Cache-Control`**: `serveStatic` never calls `setCacheControl()`/ETag → every navigation re-fetches on mobile.
- **Inline CSS/JS**: 244 `style="…"` attrs, 123 `onclick/onchange` attrs, two inline `<style>` blocks. Blocks minification, deduplication, and strict CSP.
- **Failsafe CSS duplication**: the 9 KB PROGMEM failsafe reinvents buttons/cards that already live in `style.css`.
- **Duplicated theme** in `/api/status` (13 fields/tick) + `/export_settings` — mixes rare and live data.
- **`JsonDocument` per request** (no capacity hint) for `/api/status` and `/api/live` — dynamic heap alloc per tick; fragments under sustained polling.

### Communication
- All polling, no push.
  - `/api/live` → 500 ms, 37 fields/tick.
  - `/api/recent_logs` → 3 s, reads log file from FS each poll (contends `fsMutex`).
  - Status re-polls for Wi-Fi scan, OTA reload, WiFi switch, etc.
- **SSE fits `/api/live`**: one `AsyncEventSource` channel, server pushes state on change + heartbeat.
- No conditional fetches: `/api/changelog`, `/api/platform_config` lack `If-Modified-Since` / `ETag`.

### Responsiveness
- Sidebar fixed-left ≥ 768 px, bottom nav < 768 px — breakpoint too coarse for tablets.
- Many inline `style="flex:1;min-width:200px"` overrides bypass the class system → unpredictable wrapping.
- `viewport-fit=cover` declared but no `env(safe-area-inset-bottom)` padding → iOS notch overlaps `.bottom-nav`.
- Touch targets: `.btn-sm` 6×12 px — below 44 px mobile minimum.
- Chart canvas lacks `aspect-ratio` / `max-height` — layout shift on mobile.

### Visual consistency
- `--primary` redefined (CSS vars) but also hardcoded in failsafe (`#275673`), restart page (`#27ae60`), two CSS rules (`rgba(39,86,115,.1)`).
- ~80 emojis across HTML/failsafe/`web.js`; rendered differently per OS, no dark-mode variant.
- `.btn btn-primary` coexists in main UI and failsafe with different padding/radius.
- Card spacing mixes `1rem` / `16px` / `18px` / `14px`.
- Two color systems: CSS vars (themeable) + hardcoded `#d1fae5`, `#fee2e2`, etc. in `.alert-*` — alerts break in dark mode.
- Dark mode is **not** default; `.theme-auto` only works if user has no explicit save.
- Font stack OK (system fonts, no web-font load overhead).

### Flash / RAM impact
- PROGMEM: `FAILSAFE_HTML` ~9 KB + `RESTART_HEAD/TAIL` ~1 KB ≈ **~10 KB flash** (could live on LittleFS).
- `web.js` minifies to ~45 KB, gz ~15 KB → LittleFS win ~98 KB + faster parse.
- `index.html` minifies to ~55 KB, gz ~12 KB.
- Live-poll JSON ~800 B × 2 Hz = 1.6 KB/s uplink per client; hurts with 2+ clients on AP.

---

## Pass 3 — Architecture Problems

### Fragmented UI logic
- 14 SPA pages (`#page-dashboard` … `#page-update`) all in one `index.html` — markup loads even if user visits only `/live`.
- JS is prefix-namespaced (`db*`, `live*`, `hw*`, `th*`, `net*`, `dl*`, `cl*`, `fs*`) but not modular; globals (`ST`, `CFG`, `dbChart`) leak across pages.
- Settings split 8 ways (`settings_device/flowmeter/hardware/datalog/corelogic/export/theme/network/time`) with no shared form component.
- Failsafe's Core Logic tab (`fs*` functions) duplicates the logic of `page-settings_corelogic` (`cl*` functions).

### Hardcoded HTML
- 1949 lines of static markup — any new sensor/module requires editing HTML + JS + C++ save handler.
- 244 inline `style=` + 123 inline event handlers → CSP cannot be tightened, theming leaks.
- Restart and failsafe pages inlined in PROGMEM (`WebServer.cpp:54-91`) — cannot be themed or localized, ~10 KB of flash.

### Repeated components
- `/api/status` theme block == `/export_settings` theme block (20 lines each, `WebServer.cpp:310-332` vs `595-616`).
- `save_*` handlers hand-rolled per field (`save_theme` 22 lines of near-identical `SAFE_STRNCPY`, `save_hardware` 14 lines of `toInt()`).
- `/import_settings` re-parses the same schema a third time (lines 1127–1183). Adding a field = 4 edits: struct, export, save, import.
- Defaults live in 3 places: `ConfigManager::applyDefaults()`, `/export_settings` fallback expressions, client `web.js` fallbacks.
- Setting-card grid / form-group / btn-group reimplemented per page instead of templated.

### Tight coupling backend ↔ UI
- `web.js` knows raw field names from every struct (`config.theme.primaryColor` → `primaryColor`); silent breakage on rename.
- `/api/status` payload documents required keys in a comment only (`WebServer.cpp:245-251`); no schema, no validation.
- `/api/live` returns 37 fields mixing UI, FSM state, and raw pin levels — the live page binds them directly.
- Theme polled on every status call (13 fields/tick). Should be cached + ETag-keyed.
- Server side emits theme JSON; client rebuilds `<style id='themeVars'>` from it — indirection for data that rarely changes.

### Config handling limitations
- **Two config systems, no bridge**:
  - `DeviceConfig` → binary `/config.bin`, packed + magic, non-extensible.
  - `platform_config.json` → extensible, sensor-only, disconnected.
  - Enabling a sensor in JSON does not update `config.hardware.pinFlowSensor`, and vice versa.
- **Binary config brittle**: `CONFIG_VERSION` bump needed per new field; `#pragma pack(push,1)` means any layout mistake corrupts deployed devices.
- **No namespacing**: `deviceId`, `forceWebServer`, `deviceName` sit top-level alongside nested `theme/network/hardware`.
- `_reserved_lang` byte still in struct — dead field.
- **No module registration**: sensors listed in JSON; no C++ registry for exporters/modules; no `enabled` path from UI to runtime for anything non-sensor.

### State & lifecycle
- Global singletons everywhere: `server`, `config`, `activeFS`, 113 lines of `extern` in `Globals.h`.
- `shouldRestart` set by many handlers, drained in main loop. Restart UX relies on sending HTML before shutdown; no unified pending-restart banner.
- Live polling keeps `g_lastWebActivity` fresh → prevents idle power-down; UI semantically entangled with power manager.

### Error-handling uniformity
- Save handlers return inconsistent shapes: `{"ok":true}` vs plain `"OK"` vs HTML restart page vs `{"ok":false,"error":"..."}`.
- No API versioning (`/api/v1/…`) — hard to evolve without breaking deployed UIs pinned on LittleFS.

### Observability
- No `/api/events` or `/api/metrics`; diagnostics scattered in `Serial.printf`.
- Debug toggle is a checkbox in hardware settings — no runtime log-level surface.

---

## Pass 4 — Improvement Plan (incremental, no rewrite)

Each item is sized to one landable PR. Order matters: earlier items lower risk and unlock later ones.

### A. UI Unification
- **A1.** Extract page partials to `/www/pages/<name>.html`; keep `index.html` as a thin shell (header + sidebar + bottom-nav + toast host). `nav(name)` fetches the partial and mounts into `#view`, with an in-memory cache. Initial payload drops from 89 KB to ~10 KB.
- **A2.** Split `web.js` into modules: `/www/js/core.js` (status/theme/nav/toast) and `/www/js/pages/{dashboard,live,files,sensors,settings,update}.js`. Use `<script type="module">` — no bundler.
- **A3.** Component helpers in `core.js`: `h(tag, attrs, children)` DOM builder, `Card({…})`, `StatTile({…})`, and **`Form.bind(el, schema, data)`** which renders forms from a JSON schema. Removes hand-written HTML forms.
- **A4.** Purge all inline `style=` and `onclick=`; adopt `addEventListener`. Enables strict CSP (`script-src 'self'`).

### B. Modernization (2026 aligned)
- **B1.** **Dark mode default**, Material 3-ish tokens: `--md-surface`, `--md-primary`, elevation tokens. Replace hardcoded alert colors with theme-aware vars so alerts work in dark mode.
- **B2.** Drop emojis; embed a single inline SVG sprite `/www/icons.svg` (~20 icons, ~4 KB gzipped). `<svg><use href="…#chart"/></svg>`, `currentColor` inherits theme.
- **B3.** Mobile polish: `min-height: 44px` on `.btn`/`.btn-sm`; `env(safe-area-inset-bottom)` on `.bottom-nav`; new `>=1024px` breakpoint; `aspect-ratio: 16/9` on chart canvas.

### C. Performance
- **C1.** **Gzip + Cache-Control** on static assets. Build step: `gzip -9 -k …`. Add `.setCacheControl("max-age=31536000, immutable")` for hashed assets, `max-age=60` for HTML. Expected: 220 KB → ~45 KB over wire.
- **C2.** **Minify at upload time.** Add `tools/build_web.py` (Python or esbuild) producing `dist/www/`. Keep sources in `www_src/`; upload artifacts.
- **C3.** **Split `/api/status`** into `/api/identity` (cached until save), `/api/runtime` (polled), `/api/theme` (ETag-keyed). Client caches theme with `If-None-Match`.
- **C4.** **Optional CDN UI** (ESPHome-style). Extend existing `ChartSource` concept with a `uiSource` option; if `cdn`, shell serves a tiny bootstrap that loads `web.js`/`style.css` from a hosted URL. Frees ~200 KB of LittleFS for logs.

### D. Real-time updates
- **D1.** Replace `/api/live` polling with **SSE** via `AsyncEventSource events("/api/events")`. Publish on state change plus a 1 Hz heartbeat. Client uses `new EventSource` (auto-reconnect). Fall back to polling only if EventSource fails.
- **D2.** Keep `/api/recent_logs` as pull; optionally add `?since=<bootcount>` for deltas.
- **D3.** Do **not** introduce WebSocket yet — SSE is enough and half the RAM.

### E. WebAssembly
- Not worth it for UI. The only realistic fit is heavy client-side CSV parsing for large logs (>5 MB). Skip unless `dbLoadData` JS parsing becomes a bottleneck.

### F. Minor unification wins (tiny PRs)
- Normalize every save response to `{ok, error?, restart?}`.
- Version the API: move to `/api/v1/…` behind a thin alias layer so deployed `web.js` keeps working for one release.
- Remove duplicated fallback logic in `/export_settings` by running `applyDefaults()` before serialization.
- Extract `FAILSAFE_HTML` + `RESTART_HEAD/TAIL` to LittleFS files; keep a 512-byte bootstrap in PROGMEM.

### Suggested PR sequence (≈ 4-week arc)
1. **C1 + C2** (gzip + minify) — biggest, safest win.
2. **F** bullets (API response normalization, `/api/v1` prefix).
3. **A1 + A2** (page split, module JS).
4. **B1 + B2** (dark default, SVG sprite).
5. **C3 + D1** (status split + SSE).
6. **A3 + A4** (form schema + CSP).
7. **B3** + Pass-5 items.

---

## Pass 5 — Config & Module System

### 5.1 Layer JSON on top of the binary (zero breakage)
- Keep `/config.bin` for **core only** (device/network/hardware/flowMeter). Do not churn `CONFIG_VERSION` again.
- Introduce `/config/modules.json` for per-module settings. On boot, derive it from the current `DeviceConfig` + `platform_config.json` if missing, then save.
```
/config.bin              → core (unchanged)
/config/modules.json     → NEW: modules registry + per-module settings
/config/theme.json       → NEW: theme (extract from binary over one release)
/platform_config.json    → kept; becomes one "module" (sensors)
```

### 5.2 Unified `IModule` interface (generalize `ISensor` pattern)
```cpp
class IModule {
public:
  virtual const char* getId()   const = 0;
  virtual const char* getName() const = 0;
  virtual bool load(JsonObjectConst cfg) = 0;
  virtual void save(JsonObject cfg) const = 0;
  virtual bool start() { return true; }
  virtual void stop()  {}
  virtual void tick(uint32_t nowMs) {}
  virtual bool isEnabled() const { return _en; }
  virtual void setEnabled(bool e){ _en = e; }
  virtual const char* schema() const = 0;   // PROGMEM JSON schema → drives UI forms
protected:
  bool _en = true;
};
```
Add a `ModuleRegistry` mirroring `SensorManager`. Wrap existing managers as modules in phases:

| Existing manager   | Module id      | Phase |
|--------------------|----------------|-------|
| `WiFiManager`      | `wifi`         | 1 |
| `OtaManager`       | `ota`          | 1 |
| theme/UI config    | `theme`        | 1 |
| `DataLogger`       | `datalog`      | 2 |
| `SensorManager`    | `sensors`      | 2 |
| `RtcManager`       | `time`         | 2 |
| MQTT/Webhook/…     | `export.*`     | 3 |

Old `setupXxx()` keeps working; the wrapper just routes `load/save` through JSON.

### 5.3 Generic endpoints replace per-section `save_*`
- `GET  /api/modules` → `[{id,name,enabled,hasUI}]`
- `GET  /api/modules/:id` → `{config, schema}`
- `POST /api/modules/:id` → JSON body → `load()` + persist
- `POST /api/modules/:id/enable` → hot-start if supported; else sets `shouldRestart`

Keep old `save_*` as thin wrappers for one release cycle.

### 5.4 JSON schema drives UI forms
Each module embeds a schema in PROGMEM:
```json
{ "fields":[
  {"id":"ntpServer","type":"string","max":64,"label":"NTP server"},
  {"id":"timezone","type":"int","min":-12,"max":14,"label":"Timezone"},
  {"id":"useStaticIP","type":"bool","label":"Static IP"},
  {"id":"staticIP","type":"ipv4","showIf":"useStaticIP"}
]}
```
Frontend `Form.bind('#form', schema, data)` renders all supported field types — no HTML/JS edit to add a module.

### 5.5 Wi-Fi UX
- Scan-to-select (already implemented but buried; surface in onboarding flow).
- **Captive portal on AP boot**: `AsyncDNSServer` + `/generate_204` & `/hotspot-detect.html` so phones auto-open setup.
- **"Test & save"**: `POST /api/modules/wifi/test` → STA attempt for 15 s → only persist on success.
- **Fallback countdown** visible via SSE ("Reverting to AP in 30 s…").
- Scan shows RSSI + BSSID; remember last 3 successful networks in `modules.json`.

### 5.6 OTA UX
- SSE `ota:progress` events (ETA + throughput).
- SHA-256 verify (client sends header; server checks with mbedTLS).
- Server-side magic-byte check (`0xE9`) on first chunk.
- Rollback watchdog: if firmware does not call `esp_ota_mark_app_valid()` within 60 s of boot, auto-revert. Show a "Confirm update" button for 60 s.
- Manifest URL support: `POST /api/ota/pull?url=…` (use `HttpUpdate`).
- Surface `/api/changelog` diff between current and uploaded versions.

### 5.7 System settings UX
- Collapse 8 settings sub-pages into one **Settings page with a tab strip** driven by `/api/modules` — adding a module auto-gets a tab.
- **Pending restart** banner driven by SSE + single "Restart now" button; replaces 3 inline restart pages.
- `/api/backup` zips `config.bin` + all `*.json`.
- **Factory reset scoped**: "network only" vs "all".

### 5.8 Migration path (concrete)
1. Ship `IModule` + `ModuleRegistry`, no behavior change.
2. Wrap WiFi/OTA/theme as modules; introduce `/config/modules.json`.
3. Add `/api/modules*` alongside old endpoints; ship new UI tab.
4. Port `web.js` page by page to `Form.bind(schema)`.
5. Once all pages ported, remove legacy `save_*`; drop theme fields from `DeviceConfig` (single `CONFIG_VERSION` bump, once).
6. Merge `platform_config.json` under `modules.json#sensors` with compatibility read.

### 5.9 What NOT to change
- `#pragma pack(push,1)` layout of `LogEntry` (lives in RTC RAM across deep sleep).
- Path/magic of `/config.bin`; only drain fields that move to JSON.
- Don't merge DeviceConfig sections eagerly — one section per release.

---

## Pass 6 — Failsafe Page

### Today
- `FAILSAFE_HTML` inlined at `WebServer.cpp:81-91` (~9 KB PROGMEM).
- Two tabs: Setup (upload/files/rename/control/OTA) + Core Logic (mode/sleep/sensors).
- Served when `/www/index.html` missing; always reachable at `/setup`.
- Reuses main JSON API — no API duplication.
- Problems: PROGMEM weight, inline CSS copy of main UI, emoji icons, no network-recovery surface.

### Design goals
1. **< 2 KB PROGMEM** bootstrap.
2. **Full recovery without LittleFS** (even after full wipe).
3. **Zero dependencies** — no Chart.js, no theme, no network fetches.
4. **Stable URL** `/setup` works even when main UI is healthy.
5. Progressive enhancement — no-JS upload form still works.

### 6.1 Two-stage PROGMEM layout
```
PROGMEM:     /setup_boot.html      ~500 B — minimal upload form + tiny JS
LittleFS:    /_setup/index.html    ~6 KB  — full recovery UI (shipped with firmware image)
LittleFS:    /_setup/ui.css        ~2 KB
LittleFS:    /_setup/ui.js         ~3 KB
```
`/setup_boot.html` fetches `/_setup/` first; fallback = inline upload form. Recovery bundle is flashed with firmware data partition; factory reset must not touch `/_setup/`.

### 6.2 Features the failsafe must have
**Must-have**
- FS status (free/used/total) for LittleFS + SD, with "Format LittleFS" (confirmed).
- File manager for `/`, `/www/`, `/config/`: upload/download/rename/delete.
- Bulk upload: multi-file drop, progress, per-file retry.
- Firmware OTA (already present).
- Network recovery: scan + connect, "reset network only" option, show AP SSID/IP at top.
- Device info: version, heap, chip, partitions, boot count (from `/api/status`).
- Restart + factory reset (already present) + reset-network-only.
- Serial log tail: `GET /api/logs/serial?tail=100` via a ring buffer installed in `setup()`.

**Nice-to-have**
- Config export/import (use existing `/export_settings` and `/import_settings`).
- Manual NTP/time set.
- Self-test button calling each module's `healthCheck()` hook.

### 6.3 Minimalist design
- No theming. Hardcoded:
  - `--bg: #111`, `--fg: #eee`, `--accent: #ff9800` (safety orange distinguishes from normal UI).
  - System font stack only.
- Text-first: `[upload]`, `[delete]` labels; no emojis, no SVGs.
- Single-column layout; 320 px → desktop.
- Total CSS ≤ 50 lines.

### 6.4 Bootstrap HTML sketch (≤ 512 B PROGMEM)
```html
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width">
<title>ESP32 Logger · Recovery</title>
<style>body{font:14px system-ui;margin:1em;background:#111;color:#eee}
a,button{color:#ff9800;background:#222;border:1px solid #444;padding:.5em 1em}
input{width:100%;padding:.5em;background:#222;color:#eee;border:1px solid #444}</style>
<h1>Recovery Mode</h1>
<div id=app>Loading…</div>
<script>
fetch('/_setup/index.html').then(r=>r.ok?r.text():Promise.reject())
 .then(h=>document.getElementById('app').innerHTML=h)
 .catch(()=>{document.getElementById('app').innerHTML=
  '<form method=POST enctype=multipart/form-data action=/upload?path=/_setup/>'+
  '<p>Upload recovery bundle:</p>'+
  '<input type=file name=file multiple><button>Upload</button></form>'+
  '<p><a href=/restart>Restart</a> · <a href=/factory_reset onclick="return confirm(\'Wipe?\')">Factory reset</a></p>'})
</script>
```

### 6.5 Independence guarantees
- `/setup` always returns the bootstrap regardless of FS state.
- API isolation: failsafe only uses `/api/filelist`, `/upload`, `/delete`, `/restart`, `/factory_reset`, `/do_update`, `/wifi_scan*`, `/api/status` — all registered unconditionally.
- No shared CSS/JS with main UI; different cache keys.
- Survives `/config.bin` corruption: failsafe does not read DeviceConfig for anything critical.
- Survives STA-only Wi-Fi failure: AP fallback precedes `setupWebServer()`.

### 6.6 Protecting the recovery bundle
- Allow-list check in `/delete` and `/factory_reset`: reject paths starting with `/_setup/` unless `?force_recovery=1`.
- Include `/_setup/` in the partition image (`data/_setup/`).
- `/api/recovery/verify` compares `/_setup/` to a manifest; on firmware boot, repopulate from PROGMEM if mismatched.

### 6.7 Discoverability
- Footer link on every main UI page: `<a href="/setup">Recovery</a>`.
- Long-press `WIFI_TRIGGER` ≥ 10 s → boot directly into recovery AP.
- LED heartbeat pattern differs in recovery mode.

### 6.8 Migration steps
1. Extract current `FAILSAFE_HTML` to `/www/_setup/*`. PROGMEM keeps a minimal copy as fallback. No behavior change.
2. Shrink PROGMEM bootstrap to ~500 B.
3. Add `/api/recovery/verify` + path protection on `/delete`.
4. Add "reset network only" + captive portal DNS in AP mode.
5. Add serial log ring buffer + `/api/logs/serial`.
6. Add long-press hardware trigger for recovery boot.

### 6.9 Expected wins
- PROGMEM: **~9 KB → 0.5 KB** (~8.5 KB flash saved).
- Recovery UI becomes normal HTML/CSS/JS — maintainable.
- Styling survives `/www/` corruption.

---

## Pass 7 — Bug & Code-Quality Scan

### Logical bugs (confirmed, with file:line)
- **`sanitizePath()` weak** — `Utils.cpp:19-27`. Replaces only the literal `..`; inputs like `....//secret` reduce to `../secret` and pass the leading-slash check. **Fix**: reject any remaining `..` after normalization; allow-list `[A-Za-z0-9/._-]`.
- **`sanitizeFilename()` even weaker** — `Utils.cpp:29-34`. Does not reject separators. `file=../config.bin` bypasses the `/` prefix guard.
- **`/delete` path confusion** — `WebServer.cpp:994-1013`. No root-prefix allow-list; `?storage=internal&path=/config.bin` deletes device config with one request.
- **`/delete` is GET-enabled** — `WebServer.cpp:1014`. CSRF: a stray `<img src="…/delete?path=/config.bin">` wipes data. POST-only + Origin/Referer check.
- **Upload traversal** — `WebServer.cpp:1065-1081`. `upDir` accepts `..`. `POST /upload?path=/../../` targets FS root.
- **`/save_datalog` "create" race** — `WebServer.cpp:806-839`. Opens the file without `fsMutex`; concurrent flush corrupts state.
- **`/save_platform` file-handle leak** — `WebServer.cpp:1313-1340`. Static `File s_pcfgFile` + `fsMutex` held forever if client disconnects after `index==0` but before `final`. **Fix**: tie handle to `request->_tempObject`; reset each new `index==0`.
- **`_importBuf` global String** — `WebServer.cpp:1115`. Two concurrent imports interleave bytes; no guard. `concat()` return value ignored → silent truncation under OOM.
- **`recent_logs` off-by-one** — `WebServer.cpp:456-458`. When `lCount < 5`, `idx = (lCount - 1 - i) % 5` yields wrong order on 1–4 lines.
- **`/api/regen-id`** — `WebServer.cpp:554-560`. Returns a new ID but does not persist; if the user reloads before saving, shown ID ≠ stored ID. Make it `/api/next-id` (preview) or atomic save.
- **`/set_time` leaves RTC unprotected on failure** — `WebServer.cpp:896-902`. `SetIsWriteProtected(false)` with no guard on failure path. Wrap in RAII-style scope that always re-protects.

### Memory risks (heap / stack)
- **`JsonDocument` without capacity** on hot paths: `/api/status`, `/api/live`, `/export_settings`. Heap-alloc per request; fragments under polling. **Fix**: `StaticJsonDocument<N>`.
- **Recursive `deleteRecursive`** — `Utils.cpp:36-64`. Can blow the ~4 KB AsyncTCP stack on deep trees. Convert to iterative (mirrors already-iterative `scanDir`).
- **`std::vector<String>` under `fsMutex`** — acceptable now; add a 500-entry hard cap to bound heap.
- **`_importBuf.reserve(contentLength)`** — `WebServer.cpp:1190`. A mendacious `Content-Length: 10485760` triggers a 10 MB heap alloc. Clamp to 8 KB.
- **`lastLines[5][160]` + `lineBuf[160]`** ≈ 960 B of stack in an async handler. Shrink to 128, or move to heap.
- **`Serial.printf` in upload path** — `WebServer.cpp:1082`. USB CDC disconnect can block. Use `DBGF`.

### Blocking code in async handlers
- **`delay()` in `/set_time`** — `WebServer.cpp:897-899`. Up to ~360 ms per attempt × 3 = ~1 s stalls AsyncTCP. **Fix**: queue RTC write to main loop; ACK pending.
- **5 s `fsMutex` timeouts** in many handlers — if a storage task holds the mutex, HTTP freezes. **Fix**: 500 ms + JSON `{ok:false,error:"busy"}`.
- `delay(300)` before `ESP.restart()` in `/factory_reset` — acceptable (restart follows).
- `WiFi.scanNetworks(true)` — correctly async.

### Async misuse
- **`serveStatic` only registered in the `uiReady` branch** — `WebServer.cpp:213`. Uploading `/www/index.html` at runtime does not start serving it until reboot. **Fix**: always register, or force `shouldRestart = true` after first `/www/` upload.
- **`/save_platform` body-timing assumption** — `onRequest` runs before body is fully received on some HTTP/1.0 clients. Document or verify.
- **`_tempObject` not released on client abort** — `WebServer.cpp:1051-1110`. AsyncWebServer destroys `request` but not `_tempObject`; **`fsMutex` stays held forever**. **Fix**: register `request->onDisconnect` cleanup.
- **OTA has no server-side magic-byte check** — `WebServer.cpp:1241-1247`. Any POST bytes go to `Update.write`. Check first byte `0xE9` in `onBody` when `index==0`.

### File handling (SD/LittleFS)
- **TOCTOU** on `exists(path)` → `open(path, "w")`. Harmless on LittleFS; on SD a second client could race.
- **Mixed `LittleFS` vs `activeFS`** — some handlers hardcode `LittleFS.exists("/www/index.html")` even when view == `sdcard`. Works today, confusing for future migration.
- **No disk-full check** on upload. Filling LittleFS leaves partial files.
- **Non-atomic `/config/*.json` writes** — power loss mid-write corrupts config. Write to `.new`, `fsync`, `rename`.
- **`/config.bin` saves not wrapped in `fsMutex`** in `ConfigManager` — concurrent `save_*` calls interleave writes.
- **`deleteRecursive` swallows errors** on leaves — always returns the final `rmdir` result; log per-failure.

### Security posture (defensive; LAN-trusted assumption today)
- No authentication on any endpoint. Any AP client can wipe the device. Add optional Basic Auth/token from config.
- No rate limiting. OTA spammable.
- No CSRF token on state-changing endpoints.

### Maintainability
- 3578-line `web.js` and 1949-line `index.html` are hard to review (Pass 4 addresses this).
- `CONFIG_VERSION = 12` indicates heavy churn — every bump risks bricking deployed devices (Pass 5 addresses).
- `DBG/DBGLN/DBGF` macros exist but many `Serial.print*` calls sidestep them — do a sweep.

### Top-10 fixes to land first (priority-ordered)
1. Disconnect cleanup for `/upload` and `/save_platform` — prevents `fsMutex` deadlock.
2. Harden `sanitizePath`/`sanitizeFilename`; prefix allow-list on `/delete`, `/download`, `/upload`, `/move_file`.
3. POST-only mutating endpoints; drop GET on `/delete`, `/restart`.
4. Clamp `_importBuf.reserve` to 8 KB; add mutex.
5. Server-side OTA magic-byte check.
6. Optional Basic-Auth token from config (opt-in).
7. Atomic config writes (write-temp + rename).
8. Reduce `fsMutex` timeouts to 500 ms; return `busy`.
9. Disk-full check before upload.
10. Move `/set_time` RTC writes off AsyncTCP.

### Tests worth adding
- Fuzzer for `sanitizePath` inputs.
- Soak test: 2 clients doing simultaneous `/upload` + `/save_platform` + live polling for 10 min; monitor heap/mutex.
- OTA abort mid-upload (Wi-Fi drop) → next upload succeeds.
- Power-cut mid `/config.bin` write → boot recovery.

---

## Summary — deliverables across all 7 passes

- **Pass 1**: UI & backend file map.
- **Pass 2**: performance / responsiveness / visual audit with sizing numbers.
- **Pass 3**: architecture coupling + config-system fragmentation.
- **Pass 4**: 7-PR incremental plan (gzip → module JS → SSE → module system).
- **Pass 5**: `IModule` interface, `/config/modules.json` layer, UX wins for Wi-Fi / OTA / system.
- **Pass 6**: 512-byte bootstrap + LittleFS recovery bundle design.
- **Pass 7**: 30+ concrete bugs with file:line references + top-10 priority list.

### Priority order (as per task brief)
1. **UI consistency** → Pass 4 A + B
2. **Config modularity** → Pass 5
3. **Performance** → Pass 4 C
4. **Stability** → Pass 7 top-10

### Implementation approach
Incremental, one PR at a time, no rewrites. First PRs target Pass 7 top-10 (pure bug fixes, production-safe), then Pass 4 C1/C2 (gzip + minify, biggest and safest performance win), then the module system and UI unification over subsequent releases.
