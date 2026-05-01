<div align="center">
  <h1>💧 ESP32 Logger</h1>
  <p><b>Modular, multi-sensor environmental logging platform for Seeed XIAO ESP32-C3</b></p>

  [![ESP32-C3](https://img.shields.io/badge/Board-Seeed_XIAO_ESP32--C3-blue)](#)
  [![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)](#)
  [![C++](https://img.shields.io/badge/Language-C++-00599C?logo=c%2B%2B)](#)
  [![SPA](https://img.shields.io/badge/Frontend-Vanilla_JS_SPA-F7DF1E)](#)
  [![LittleFS](https://img.shields.io/badge/Storage-LittleFS_%2B_SD-darkgreen)](#)
  [![CSP](https://img.shields.io/badge/Security-Strict_CSP-success)](#)
  [![WCAG](https://img.shields.io/badge/A11y-WCAG_2.2-informational)](#)
</div>

---

## 📌 Project Overview

**ESP32 Logger** started as a low-power water usage tracker and has evolved into a full **multi-sensor environmental sensing platform (v5.0)**. It combines the original deep-sleep water logger with a FreeRTOS task pipeline that can run up to 8 sensor types simultaneously, log data locally in JSON Lines format, and push readings to multiple cloud or home-automation destinations.

Three operating modes let you keep existing behavior or opt into the new pipeline at your own pace — with full backward compatibility guaranteed.

The web UI has been rebuilt into an **industrial-IoT dashboard** (Claude Design handoff): Lucide SVG icons, dark/light/auto theme tokens, collapsible sidebar rail, keyboard shortcuts, and WCAG 2.2-grade accessibility. The firmware itself was hardened across the audit's seven passes — strict CSP, atomic writes, JSON input caps, token-bucket rate limit, CSRF tokens on `/save_*`, OTA SHA-256 verify with rollback watchdog, and a captive-portal DNS responder for one-tap AP onboarding.

- **Author:** Petko Georgiev

---

## ⚙️ Key Features

### 🔋 Flexible Operating Modes

| Mode | Description | Deep Sleep |
|---|---|---|
| `legacy` | Original water logger (default, unchanged behavior) | YES |
| `continuous` | Multi-sensor FreeRTOS pipeline, no deep sleep | NO |
| `hybrid` | Water logger + sensor pipeline running together | NO |

Mode is selected by editing `platform_config.json` via the **Core Logic** settings page. First-boot default is `legacy` — no action required to keep existing behavior.

### 🌊 Water Flow Detection & Post-Correction (Legacy / Hybrid)
- Full Flush (FF) and Part Flush (PF) triggers via hardware interrupts.
- **Smart Post-Correction:** auto-corrects FF↔PF if the measured volume crosses configurable thresholds.
- Intentional long-press bypass for manual overrides.
- Deep-sleep between events for ultra-low power consumption.

### 🌡️ Multi-Sensor Plugin Architecture (v5.0)

Eight sensor plugins, each implementing the `ISensor` interface and registered at boot:

| Plugin | Interface | Metrics |
|---|---|---|
| **BME280** | I2C | Temperature, Humidity, Pressure |
| **SDS011** | UART | PM2.5, PM10 |
| **PMS5003** | UART | PM1.0, PM2.5, PM10 |
| **YF-S201 / YF-S403** | Pulse (ISR) | Flow Rate (L/min), Volume (L) |
| **ENS160** | I2C | AQI, TVOC (ppb), eCO2 (ppm) |
| **SGP30** | I2C (raw, no lib) | TVOC (ppb), eCO2 (ppm) |
| **Rain Gauge** | Pulse (ISR) | Rain Rate, Rain Total (mm) |
| **Wind Speed** | Pulse (ISR) | Wind Speed (m/s) |

Adding a new sensor requires only implementing `ISensor` and one `registerPlugin()` call in `Logger.ino`.

### ⚡ FreeRTOS Task Pipeline
Four tasks connected by queues, all pinned to core 0 (unicore RISC-V):

```
SensorTask (prio 3) ──► ProcessingTask (prio 2) ──┬──► StorageTask (prio 1)
                         validate · normalize       └──► ExportTask  (prio 1)
                         aggregate · ring-buf
```

- **SensorTask** — ticks all enabled plugins at their configured intervals.
- **ProcessingTask** — range-validates readings, populates the lock-free ring buffer for the web UI, routes to storage and export queues.
- **StorageTask** — appends `SensorReading` structs as JSON Lines to daily rotating files.
- **ExportTask** — dispatches to all enabled exporters with exponential-backoff retry.

### 💾 Hybrid Storage (SD + LittleFS)
- **SD present:** primary storage (`/logs/YYYY-MM-DD.jsonl`), LittleFS used as fast-read cache.
- **SD absent:** LittleFS-only fallback with automatic rotation.
- **`cloud_only` flag:** skips local writes when disk space is not a concern.
- Raw JSON Lines data is **never mutated** — aggregation is read-time only.

### 📊 Aggregation Engine (LTTB + Time Buckets)
- **LTTB** (Largest Triangle Three Buckets, Steinarsson 2013) — reduces thousands of points to a configurable limit while preserving visual shape.
- **Time buckets:** raw / 1 min / 5 min / 1 hour / 1 day.
- **Aggregation modes:** RAW, AVG, MIN, MAX, LTTB, SUM — selectable per API call and per chart.

### 📡 Multi-Destination Export
| Exporter | Protocol | Details |
|---|---|---|
| **MQTT** | PubSubClient | Topic per sensor+metric, configurable prefix, QoS, retain |
| **HTTP POST** | ESP32 HTTPClient | Generic JSON array body, optional `Authorization` header |
| **Sensor.Community** | HTTP | X-Pin headers per sensor type, rate-limited |
| **openSenseMap** | HTTP | Bearer token, per-metric sensor ID mapping |

All exporters are configured from the **Export** settings page and stored in `platform_config.json`.

### 📱 Web Interface (SPA) — Industrial-IoT Redesign
The single-page application has been rewritten as an industrial-IoT dashboard with a complete design-system foundation.

**Design system**
- Token-based color/typography pairing (`--d-*` namespace): Inter for UI, JetBrains Mono for telemetry.
- Light / Dark / Auto theme with early-paint bootstrap (no flash on dark systems).
- Density toggle (`[data-density="compact"]`) and accent colorways (cyan / amber / green / violet).
- Material-3-inspired surface elevation and 6 px border radius.

**Iconography**
- Lucide 0.441 SVG icon set inlined as `/www/js/icons.js` (no CDN dependency).
- 30+ icons covering nav, settings, OTA, sensor states, modules.
- All icons inherit `currentColor`, so dark/light themes "just work".

**Navigation**
- Collapsible sidebar **rail mode** (60 px icon-only) on desktop — preference persists.
- Bottom-nav on mobile with WCAG 2.5.8 44 px touch targets and iOS safe-area inset.
- Keyboard shortcuts: <kbd>G</kbd> <kbd>D/L/F/C/S/U</kbd> for top-level pages, <kbd>?</kbd> for help, <kbd>Esc</kbd> to close.

**Pages**
- **Dashboard** — chart with crosshair tooltip, Liters/Events stat tiles, threshold filter, CSV export.
- **Live** — SSE-driven event log with **free-text filter**, button-state badges, recent flow events.
- **Files** — folder browser, upload, mkdir, move, delete; structured empty states.
- **Sensors** — sensor card grid with **staleness amber/red bands** and relative-age pills (`· 5 m ago`), plus the Core Logic editor.
- **Settings** — Device, Network (WiFi scan + credential test), Time (RTC + NTP), Theme, Datalog, Hardware, Export, Modules, Core Logic.
- **Update** — OTA upload with hash + verify status (`Hashing… → Uploading… → Verifying… → Update complete`).

**Accessibility (WCAG 2.2)**
- Programmatic skip-link → focuses the active page (no SPA hash-router collision).
- Global `:focus-visible` outline (3 px accent, 2 px offset).
- `prefers-reduced-motion` kills all animations.
- 44 px minimum touch targets across nav, buttons, and form inputs.

**Toast system**
- Replaces every `alert()` call. Title + body + Lucide icon + countdown bar + dismiss button.
- Four severity classes (`ok` / `warn` / `err` / `info`) with `role="alert"` on errors.

### 🧩 Module System (Pass 5)
- `IModule` + `ModuleRegistry` interface plus per-module JSON shadow at `/config/modules.json` (atomic write/rename).
- WiFi · OTA · Theme · DataLog · Time modules ship with PROGMEM JSON schemas that drive `Form.bind()` in the Settings UI.
- Generic CRUD: `GET/POST /api/modules/:id`, `POST /api/modules/:id/enable?on=0|1`.
- WiFi-specific helpers: `GET /api/modules/wifi/scan` (async, polling), `POST /api/modules/wifi/test` (FreeRTOS worker, non-blocking on AsyncTCP).

### 📡 Captive Portal (Pass 5 5.5 phase 2)
- Wildcard DNS responder bound to UDP/53 in AP mode resolves every host to the AP IP.
- HTTP redirect handlers for the OS-level captive-portal probes (Apple `/hotspot-detect.html`, Android `/generate_204`, Windows `/connecttest.txt` etc.) emit absolute redirects to the SPA root.
- Net effect: when a phone joins the `WaterLogger` AP, the OS auto-opens the config page within ~2 s — no need to type the IP.

### 🛟 Resilient Failsafe Recovery
- If `/www/index.html` is missing or corrupt, the firmware serves a hardcoded failsafe UI.
- `/setup` is always reachable for drag-and-drop file recovery — **Core Logic** page is also available here.
- OTA firmware update with `.bin` magic byte validation, **SHA-256 verification**, and rollback watchdog (see [Security & Hardening](#-security--hardening)).

---

## 🔌 Hardware Requirements & Default Pins

Designed for **Seeed Studio XIAO ESP32-C3 (RISC-V)**. Generic ESP32 dual-core is also supported (see `platformio.ini`).

| Component | Default Pin | Notes |
|---|---|---|
| **WiFi Trigger** | D0 (GPIO 2) | Wake to force AP / Web Server |
| **Wakeup FF** | D1 (GPIO 3) | Full Flush button |
| **Wakeup PF** | D2 (GPIO 4) | Part Flush button |
| **Flow Sensor (YF-S201)** | D6 (GPIO 21) | Pulse ISR |
| **RTC (DS1302)** | D3/D4/D5 | CE, IO, SCLK |
| **SD Card SPI** | D7/D8/D9/D10 | Configurable in Hardware settings |
| **BME280 / ENS160 / SGP30** | SDA D4, SCL D5 | I2C, address in `platform_config.json` |
| **SDS011 / PMS5003** | Configurable UART | RX/TX pins in `platform_config.json` |
| **Rain / Wind** | Configurable pulse | GPIO pin in `platform_config.json` |

---

## 🚀 Build & Flash

### PlatformIO (recommended)

```bash
# Primary target — XIAO ESP32-C3
pio run -e xiao_esp32c3 -t upload

# Generic ESP32 dual-core
pio run -e esp32 -t upload

# Upload LittleFS (web UI + default config)
pio run -e xiao_esp32c3 -t uploadfs
```

### Arduino IDE

1. Select board **XIAO ESP32-C3** (or ESP32 with matching partition scheme).
2. Compile and flash `Logger.ino`.
3. Use the ESP32 LittleFS upload plugin to flash the `/data` folder (contains `www/` and `platform_config.json`).

### First Boot

1. Connect to the `WaterLogger` AP (or its IP after joining your network).
2. The failsafe recovery page appears if `/www/index.html` is not yet uploaded.
3. Upload `www/index.html`, `www/web.js`, `www/style.css` (and optionally `www/changelog.txt`).
4. The full SPA loads. The device runs in **legacy** mode by default — no sensors or exporters are active until you configure them.

---

## 🗂 Config Files

| File | Format | Purpose |
|---|---|---|
| `/config.bin` | Binary v12 | Legacy device config (pins, thresholds, Wi-Fi). **Unchanged.** |
| `/platform_config.json` | JSON | Sensors, aggregation, exporters, storage, operating mode. Human-editable. |
| `/logs/YYYY-MM-DD.jsonl` | JSON Lines | Immutable raw sensor readings. One file per day. |

`platform_config.json` is backward-compatible: the file is optional. If absent, the device boots in legacy mode.

### Minimal `platform_config.json`

```json
{
  "version": 1,
  "mode": "continuous",
  "sensors": [
    {
      "id": "env_indoor",
      "type": "bme280",
      "enabled": true,
      "interface": "i2c",
      "sda": 6,
      "scl": 7,
      "address": 118,
      "read_interval_ms": 10000
    }
  ],
  "aggregation": {
    "default_mode": "lttb",
    "default_bucket_min": 5,
    "max_points": 500
  }
}
```

---

## 🌐 Local API Reference

All routes are also reachable under **`/api/v1/<X>`** via a 307-redirect alias, so clients can adopt versioned URLs without breaking the deployed UI.

### Sensor data & state
| Endpoint | Method | Description |
|---|---|---|
| `/api/data` | GET | Query sensor readings with aggregation (params below) |
| `/api/sensors` | GET | List all registered sensors with `status` + `read_interval_ms` |
| /api/sensors/read_now?id=<id> | GET | Read a single non-blocking sensor by ID |
| `/api/recent_logs` | GET | Recent flow-event log; `?since=<bootcount>` returns delta |
| `/api/diag` | GET | Free heap, queue depths, task stats |
| /api/identity / /api/runtime / /api/theme | GET | Split status (identity / runtime / theme) |
| `/api/changelog` | GET | Markdown changelog from `/changelog.txt` |

### Modules (Pass 5)
| Endpoint | Method | Description |
|---|---|---|
| `/api/modules` | GET | Index of registered modules |
| `/api/modules/:id` | GET / POST | Detail + schema / update |
| `/api/modules/:id/enable?on=0\|1` | POST | Hot enable/disable; reports `restartRequired` |
| `/api/modules/wifi/scan` | GET | Async wildcard scan (polling) |
| `/api/modules/wifi/test` | POST + GET | Probe credentials in worker task; GET polls result |

### Config & backup
| Endpoint | Method | Description |
|---|---|---|
| `/api/platform_config` | GET | Fetch current `platform_config.json` |
| `/save_platform` | POST | Save updated `platform_config.json` (raw JSON body) |
| `/api/platform_reload` | POST | Apply new config and restart |
| `/api/backup` | GET | Full-state JSON snapshot (modules + sensors + platform); `Content-Disposition` filename includes deviceId + bootCount |
| `/export_settings` | GET | Legacy core config export |
| `/api/csrf-token` | GET | Per-boot CSRF token (32 hex), required by `/save_*` mutating routes |

### OTA
| Endpoint | Method | Description |
|---|---|---|
| `/do_update?sha256=<64-hex>` | POST | Stream firmware to OTA partition; SHA-256 verify before commit |
| `/api/ota/status` | GET | Running/previous partition labels, `pending_verify`, `confirm_in_ms` |
| `/api/ota/confirm` | POST | Manually mark current image valid (cancels rollback) |
| `/api/ota/rollback` | POST | Roll back to previous slot and reboot |

### `GET /api/data` Parameters

| Param | Values | Default |
|---|---|---|
| `from` | Unix timestamp | now − 24 h |
| `to` | Unix timestamp | now |
| `sensor` | sensor id | all |
| `metric` | metric name | all |
| `agg` | `raw` / `1m` / `5m` / `1h` / `1d` | `5m` |
| `mode` | `raw` / `avg` / `min` / `max` / `lttb` | `lttb` |
| `limit` | 1 – 5000 | `500` |

---

## 🔒 Security & Hardening

The audit's seven-pass sweep was applied across the firmware. Highlights:

- **Strict CSP** (`script-src 'self'`) — every inline `on*` and `style=` handler removed; event dispatcher in `core.js` routes `data-click` / `data-change` / `data-submit` / `data-input` / `data-error` through a vetted handler registry.
- **Rate-limit token bucket** on every mutating route (20 tokens, +5/sec); over-budget callers get `HTTP 429 Retry-After: 1`.
- **CSRF tokens** on `/save_*` — per-boot 128-bit token from the hardware RNG, served at `GET /api/csrf-token`, validated in constant time via the `csrf` form/query field. Mismatch → `HTTP 403`.
- **Atomic JSON writes** — `/config/modules.json` writes go through `<file>.new` → fsync → rename. A power loss between close and rename leaves the previous file intact; a stale `.new` is GC'd on next boot.
- **JsonDocument input caps** — `ExportManager`, `SensorManager`, `HybridStorage` refuse JSON files > 8/16 KB so a corrupted/crafted config can't OOM the heap.
- **fsMutex 500 ms timeout** on `/delete`, `/mkdir`, `/move_file` — return `HTTP 503` instead of starving the AsyncTCP worker.
- **RAII guard** around RTC write-enable in `/set_time`.
- **Heap-only buffers** in `/api/recent_logs` so the AsyncTCP worker stack stays under budget.

### OTA flow

1. Client computes SHA-256 of the `.bin` in-browser (SubtleCrypto), POSTs to `/do_update?sha256=<digest>`.
2. Server streams chunks through both `Update.write()` and an mbedTLS hasher (per-request `_tempObject`, freed via `request->onDisconnect` if the upload aborts).
3. Magic-byte (`0xE9`) check + final-chunk hash compare. Mismatch → `Update.abort()` + `HTTP 400`.
4. Bootloader marks the new image `PENDING_VERIFY`. The firmware confirms only after **`OTA_CONFIRM_TIMEOUT_MS` (90 s)** of stable operation, OR before any deliberate sleep / reboot path (so legacy mode's ~2 s wake/sleep cycle never trips a false rollback).
5. A panic, watchdog, or brownout reset before confirmation triggers an automatic bootloader rollback to the previous slot on next boot.

`/api/ota/status` surfaces `confirm_in_ms` so the UI can render a "Confirming in N s" countdown.

---

## 🧰 Build Pipeline

`tools/build_web.py` produces a flash-ready `dist/www/` from the source `www/` tree:

```bash
python3 tools/build_web.py [--clean]
# upload dist/www/ to LittleFS via the SPA's /upload page
```

| Asset | Transform | Result |
|---|---|---|
| `*.html` | conservative regex minify (protects `<pre>`/`<style>`/quoted strings), gzip -9 | ~25-30 % wire size |
| `*.css` | comment + whitespace strip with quote-stash, gzip -9 | ~30 % wire size |
| `*.js` / `*.json` / `*.txt` | gzip -9 only (no parser-free JS minify) | ~30-50 % wire size |
| binary (`*.png` / `*.ico` / `*.woff` …) | passed through | unchanged |

Both the minified plain file and its `.gz` sibling are written; `serveStatic()` probes `.gz` first and emits `Content-Encoding: gzip`. Net wire size for the current tree: **530 KB → 208 KB (39 %)** on a gzip-aware browser.

### Optional CDN UI (`-DUI_CDN_BASE`)

Compile-time opt-in: build with `-DUI_CDN_BASE="\"https://cdn.example.com/v4.2.0\""` and the root `/` handler emits a 1 KB bootstrap that loads `index.html` + `style.css` + `js/*.js` from the CDN. Frees ~200 KB of LittleFS for log storage. `?_local=1` falls back to the on-device copy when the CDN is unreachable.

---

## 🛠 Tech Stack

| Layer | Technology |
|---|---|
| Firmware | Arduino Framework, FreeRTOS (ESP32-C3 / RISC-V) |
| Web Server | ESPAsyncWebServer + AsyncTCP |
| Serialization | ArduinoJson |
| Sensors | Adafruit BME280, PubSubClient (MQTT), raw I2C/UART |
| Storage | LittleFS, SD (SPI), JSON Lines |
| Frontend | HTML5, Vanilla JS SPA, Chart.js, CSS3 |
| RTC | RtcDS1302 (Makuna) |
| Build | PlatformIO (primary), Arduino IDE (supported) |

---

## 📁 Source Layout

```
ESP32_Logger/
├── Logger.ino                  ← entry point; platform init + main loop ticks
│                                  (OtaManager::tick, tickCaptivePortalDNS, …)
├── platformio.ini
├── ARCHITECTURE.md             ← full architecture, class designs, API reference
├── Audit_report_17042026.md   ← seven-pass audit + improvement plan
├── tools/
│   ├── build_web.py            ← minify + gzip pipeline (Pass 4 C2)
│   └── gzip_www.sh             ← legacy wrapper (kept for compat)
├── www/                        ← SPA source of truth (hand-edited)
│   ├── index.html
│   ├── style.css
│   ├── pages/                  ← settings_*.html partials (lazy-loaded)
│   └── js/
│       ├── theme-boot.js       ← head-loaded, no-FOUC theme + sidebar-rail restore
│       ├── core.js             ← bootstrap, dispatcher, toasts, kbd shortcuts, CSRF
│       ├── icons.js            ← Lucide 0.441 SVG path bank + Icons.swap()
│       ├── pages.js            ← dashboard, files, live (chart crosshair, log filter)
│       ├── settings.js         ← settings sub-pages, changelog, OTA + SHA-256
│       └── sensors.js          ← sensors grid (staleness pills), Core Logic editor
├── dist/                       ← .gitignored — build_web.py output
└── src/
    ├── core/                   ← Globals, IModule, ModuleRegistry, atomic write
    ├── modules/                ← WiFi · OTA · Theme · DataLog · Time module adapters
    ├── managers/               ← ConfigManager, WiFiManager (+ captive portal),
    │                              StorageManager, RtcManager, OtaManager (rollback),
    │                              HardwareManager, DataLogger
    ├── sensors/                ← ISensor + SensorManager + 8 plugins
    ├── pipeline/               ← DataPipeline queues, AggregationEngine (LTTB)
    ├── storage/                ← JsonLogger (JSON Lines), HybridStorage (SD+LFS)
    ├── export/                 ← IExporter, MQTT / HTTP / SC / OSM exporters
    ├── tasks/                  ← TaskManager, 4 FreeRTOS tasks
    └── web/                    ← WebServer, ApiHandlers, RateLimiter, CsrfToken
```

For a deep dive into every class, queue, mutex, and API payload see [`ARCHITECTURE.md`](ARCHITECTURE.md). For the audit findings + improvement plan that drove the recent passes, see [`Audit_report_17042026.md`](Audit_report_17042026.md).

---

This project is maintained in free time. If it saved you development hours, consider supporting it.
<p align="center">
  <a href="https://revolut.me/petk0g">
    <img src="https://img.shields.io/badge/Support-Revolut-0666EB?style=for-the-badge&logo=revolut&logoColor=white" />
  </a>
</p>
