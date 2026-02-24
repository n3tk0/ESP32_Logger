# 💧 ESP32 Low-Power Water Usage Logger

Low-power multi-sensor water usage logger for **Seeed XIAO ESP32-C3 (RISC-V)** with deep sleep wake-up, modular firmware, SPA web UI, and resilient recovery workflow.

---

## 📌 Project Overview

- **Project:** ESP32 Low-Power Water Usage Logger
- **Release Line:** 4.1.x (latest web stack updates aligned with v4.1.5 changelog)
- **Target Board:** Seeed Studio XIAO ESP32-C3
- **Author:** Petko Georgiev
- **Organization:** Villeroy & Boch Bulgaria

Firmware goals:
- Accurate PF/FF flush event logging
- Ultra low-power behavior via deep sleep
- Reliable startup button detection
- Robust web-based configuration and file management
- Recovery path even when web UI files are missing/corrupted

---

## ⚙️ Core Features

### 🔋 Low Power & Reliability
- GPIO wake-up + deep sleep cycle
- Early GPIO snapshot during boot (improves wake source detection)
- Configurable debounce and edge filtering
- Safe restart path with WiFi hardware shutdown (`safeWiFiShutdown()`)

### 🚽 PF/FF Detection + Post-Correction
- Multi-button flush identification (PF/FF)
- Optional volume-based post-correction:
  - PF → FF when volume exceeds threshold
  - FF → PF when volume is below threshold
- Hold-time guard (`manualPressThresholdMs`) to skip correction for intentional long presses
- Post-correction events logged to `btn_log.txt`

### 💾 Storage
- LittleFS (default) or SD Card
- Rotation modes and formatting options for datalog output
- Wake and sleep timestamps in log lines

---

## 🌐 Web UI (SPA + Recovery)

### Main Behavior
- Front-end is served from **LittleFS `/www/`** (`index.html`, `web.js`, `style.css`)
- Legacy routes redirect to SPA root (`/`)
- Runtime and configuration are separated:
  - `/api/status` for live state
  - `/export_settings` for complete config payload

### Recovery & Failsafe
- If `/www/index.html` is missing, device serves embedded failsafe page
- `/setup` route is **always available** for emergency UI recovery uploads

### Additional Improvements
- Chart.js loader supports local path or CDN fallback
- CSV export reflects “Exclude 0.00L” filter in filename suffix
- Network settings load status/config separately with password-safe behavior

---

## 🔌 Default Pins (XIAO ESP32-C3)

| Function      | Default Pin |
|---------------|-------------|
| WiFi Trigger  | D0 (GPIO 2) |
| Wakeup FF     | D1 (GPIO 3) |
| Wakeup PF     | D2 (GPIO 4) |
| Flow Sensor   | D6 (GPIO 21) |
| RTC (DS1302)  | GPIO 5/6/7 |
| SD Card SPI   | GPIO 10–13 |

---

## 📊 API Endpoints (selected)

| Endpoint | Purpose |
|----------|---------|
| `/api/status` | Live runtime status |
| `/export_settings` | Full settings payload for UI |
| `/api/recent_logs` | Last log entries |
| `/api/changelog` | Changelog text from LittleFS |
| `/api/filelist` | File list for selected storage |
| `/setup` | Always-on failsafe recovery page |

---

## 🚀 Flashing / Deployment

1. Build and flash firmware (`Logger.ino`).
2. Upload LittleFS content including:
   - `/www/index.html`
   - `/www/web.js`
   - `/www/style.css`
   - `/www/changelog.txt`
   - optional assets (`/chart.min.js`, logo/favicon)
3. If UI fails to load, open `http://<device-ip>/setup` and re-upload UI files.

---

## 🛠 Tech Stack

- Arduino Framework (ESP32)
- ESPAsyncWebServer + AsyncTCP
- LittleFS / SD
- ArduinoJson
- RtcDS1302
- FlowSensor library

---

## 📄 License

Internal / Custom project.
