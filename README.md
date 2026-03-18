<div align="center">
  <h1>💧 ESP32 Logger</h1>
  <p><b>Modular, multi-sensor environmental logging platform for Seeed XIAO ESP32-C3</b></p>

  [![ESP32-C3](https://img.shields.io/badge/Board-Seeed_XIAO_ESP32--C3-blue)](#)
  [![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)](#)
  [![C++](https://img.shields.io/badge/Language-C++-00599C?logo=c%2B%2B)](#)
  [![SPA](https://img.shields.io/badge/Frontend-Vanilla_JS_SPA-F7DF1E)](#)
  [![LittleFS](https://img.shields.io/badge/Storage-LittleFS_%2B_SD-darkgreen)](#)
</div>

---

## 📌 Project Overview

**ESP32 Water Logger** started as a low-power water usage tracker and has evolved into a full **multi-sensor environmental sensing platform (v5.0)**. It combines the original deep-sleep water logger with a FreeRTOS task pipeline that can run up to 8 sensor types simultaneously, log data locally in JSON Lines format, and push readings to multiple cloud or home-automation destinations.

Three operating modes let you keep existing behavior or opt into the new pipeline at your own pace — with full backward compatibility guaranteed.

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

### 📱 Extended Web Interface (SPA)
The single-page application gains three new sections while keeping all existing pages:

**🌡️ Sensors — Live Dashboard**
- Auto-refreshing sensor card grid (type, last value, metrics, status badge).
- Integrated Chart.js chart with sensor / metric / time-range / aggregation selectors.

**🧩 Core Logic — Settings**
- Platform mode selector (legacy / continuous / hybrid).
- Sensor list: enable/disable, add, edit (JSON), remove — config saved to `platform_config.json`.
- Aggregation defaults (bucket, mode, max points, retention days).
- Export quick-enable toggles.
- Accessible from both the main settings page and the **failsafe recovery page**.

**📡 Export — Settings**
- Full configuration forms for all four exporters (MQTT broker, HTTP endpoint, Sensor.Community, openSenseMap box + sensor ID mapping).

**📈 Datalog — Aggregation Card**
- Aggregation interval and interpolation mode dropdowns with inline help tips.
- Preferences saved to `localStorage` and applied automatically to chart queries.

### 🛟 Resilient Failsafe Recovery (unchanged)
- If `/www/index.html` is missing or corrupt, the firmware serves a hardcoded failsafe UI.
- `/setup` is always reachable for drag-and-drop file recovery — **Core Logic** page is also available here.
- OTA firmware update with `.bin` magic byte validation.

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

| Endpoint | Method | Description |
|---|---|---|
| `/api/data` | GET | Query sensor readings with aggregation |
| `/api/sensors` | GET | List all registered sensors and their status |
| `/api/platform_config` | GET | Fetch current `platform_config.json` |
| `/save_platform` | POST | Save updated `platform_config.json` (JSON body) |
| `/api/platform_reload` | POST | Apply new config and restart |

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
Water_logger/
├── Logger.ino                  ← entry point; platform init wired here
├── platformio.ini
├── ARCHITECTURE.md             ← full architecture, class designs, API reference
├── docs/
│   └── MIGRATION_PLAN.md
├── www/                        ← SPA assets (upload to LittleFS)
│   ├── index.html
│   ├── web.js
│   ├── style.css
│   └── platform_config.json   ← default config (deployed with FS upload)
└── src/
    ├── core/                   ← SensorTypes, Globals (existing + new)
    ├── managers/               ← ConfigManager, HardwareManager, etc. (UNCHANGED)
    ├── sensors/                ← ISensor interface, SensorManager, 8 plugins
    ├── pipeline/               ← DataPipeline queues, AggregationEngine (LTTB)
    ├── storage/                ← JsonLogger (JSON Lines), HybridStorage (SD+LFS)
    ├── export/                 ← IExporter, MQTT / HTTP / SC / OSM exporters
    ├── tasks/                  ← TaskManager, 4 FreeRTOS tasks
    └── web/                    ← WebServer (extended), ApiHandlers (/api/data, /api/sensors)
```

For a deep dive into every class, queue, mutex, and API payload see [`ARCHITECTURE.md`](ARCHITECTURE.md).

---

This project is maintained in free time. If it saved you development hours, consider supporting it.
<p align="center">
  <a href="https://revolut.me/petk0g">
    <img src="https://img.shields.io/badge/Support-Revolut-0666EB?style=for-the-badge&logo=revolut&logoColor=white" />
  </a>
</p>
