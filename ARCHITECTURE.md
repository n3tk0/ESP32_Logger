# 💧 ESP32 Water Logger - System Architecture

This document provides a comprehensive architectural overview of the **ESP32 Water Logger**, detailing the modular C++ backend, the Single Page Application (SPA) frontend, and the data flow between them.

---

## 🏗️ 1. System Overview

The ESP32 Water Logger has transitioned from a legacy monolithic structure into a highly organized, modular architecture. This separation of concerns improves maintainability, scalability, and memory efficiency.

The system is strictly divided into two primary domains:
1. **The Backend (ESP32 Firmware):** Written in C++, compiled via Arduino IDE or PlatformIO. It handles hardware interrupts, sensor reading, non-volatile storage (LittleFS/SD), real-time clock (RTC) management, and serves a lightweight asynchronous HTTP API.
2. **The Frontend (Web UI):** A Single Page Application (SPA) built purely with HTML, Vanilla JavaScript, and CSS. It is served directly from the ESP32's LittleFS partition (`/www/`) to the client's browser, communicating with the backend exclusively via JSON API endpoints.

---

## 📁 2. Directory & File Structure

The project strictly separates the C++ logic from the UI assets.

```text
Water_logger/
├── Logger.ino               ← The main entry point (setup/loop), state machine, and deep-sleep logic.
├── README.md                ← High-level project documentation.
├── ARCHITECTURE.md          ← This file.
│
├── www/                     ← 🌐 FRONTEND: Assets served to the browser (Upload to LittleFS)
│   ├── index.html           ← The single HTML file containing all UI views/templates.
|   ├── changelog.txt        ← Project release notes.
│   ├── web.js               ← Vanilla JS handling routing, DOM updates, and API calls.
│   ├── style.css            ← Vanilla CSS defining the custom design system and layout.
│   └── chart.min.js         ← Local dependency for rendering volume charts.
│
└── src/                     ← ⚙️ BACKEND: Modular C++ source code
    ├── core/
    │   ├── Config.h         ← Global structs (Theme, Datalog, Hardware, etc.) and Enums.
    │   └── Globals.h/.cpp   ← Global state variables (pulseCount, loggingState, pointers).
    │
    ├── managers/            ← Isolated classes handling specific domain logic
    │   ├── ConfigManager    ← Serializes/Deserializes configuration to/from LittleFS.
    │   ├── WiFiManager      ← Connects to AP/STA, manages Network status and RSSI.
    │   ├── StorageManager   ← Abstracts LittleFS vs. SD Card operations.
    │   ├── RtcManager       ← Interfaces with the DS1302 hardware clock via Makuna lib.
    │   ├── HardwareManager  ← GPIO setup, button debouncing, interrupts.
    │   └── DataLogger       ← Formats and appends data to CSV log files + log rotation.
    │
    ├── web/
    │   └── WebServer.h/.cpp ← Defines ESPAsyncWebServer routes (Static files & REST API).
    │
    └── utils/
        └── Utils.h/.cpp     ← Shared helper functions (formatBytes, Time strings, etc.).
```

---

## ⚙️ 3. Backend Architecture (C++ Modular)

The backend is driven by `Logger.ino`, which initializes the managers and orchestrates the core state machine.

### Manager Pattern
Instead of dumping all logic into `.ino`, the code uses isolated Manager Singleton/Static Classes:
- `ConfigManager.begin()`: Loads `/config.bin`.
- `HardwareManager.begin()`: Attaches interrupts to the Flow Sensor (`pinFlowSensor`) and wakeup buttons (`pinWakeupFF`, `pinWakeupPF`).
- `WiFiManager.begin()`: Establishes network connectivity based on config (AP or Client).
- `WebServer.begin()`: Starts listening on port 80.

### Main State Machine (`loop()`)
The `loop()` function runs extremely fast (non-blocking) and handles the water logging lifecycle:
1. **`STATE_IDLE`**: Waiting for a hardware trigger (Flow pulse or flush button).
2. **`STATE_WAIT_FLOW`**: A button was pressed, waiting for water to start flowing.
3. **`STATE_MONITORING`**: Water is flowing. Accumulating `pulseCount`.
4. **`STATE_DONE`**: Flow stopped (timeout reached). The backend compiles the data, calculates liters, and hands it to `DataLogger` to encode and write to the active storage medium.

### Memory & Async Web Server
The backend uses `ESPAsyncWebServer` and `AsyncTCP`. This allows the ESP32 to serve files and respond to API requests asynchronously on a separate FreeRTOS task, preventing the UI from blocking the critical flow-meter pulse interrupts.

---

## 🖥️ 4. Frontend Architecture (Web UI)

The frontend is a lightweight Single Page Application (SPA), designed to minimize memory overhead on the ESP32 while providing a modern, rich aesthetic.

### Hash Routing
Instead of loading multiple HTML files from the ESP32 (which is slow), `index.html` loads once. Navigation is handled by `web.js` listening to DOM hash changes (e.g., `#dashboard`, `#settings_theme`). The JS selectively adds/removes the `.active` CSS class to show/hide `<main class="page">` blocks.

### Theming System
The UI supports dynamic theming (Light/Dark/Auto). Colors are derived from the backend API `/export_settings` payload (`config.theme.primaryColor`, etc.) and injected dynamically into the `<style id="themeVars">` block as CSS Custom Properties (`--primary`, `--bg`, etc.).

### Robustness & Failsafe
To prevent bricking the UI:
- `WebServer.cpp` serves a hardcoded "Failsafe UI" if `LittleFS` is empty or `/www/index.html` is missing. This failsafe page allows the user to upload the `www/` folder contents cleanly to restore the system.
- OTA Updates (Firmware `.bin` uploads) are handled via a dedicated dropzone with progress bars, parsing the JSON response safely.

---

## 🔄 5. Data Flow & API Endpoints

The frontend never submits traditional HTML `<form>` POSTs replacing the page. Instead, `web.js` intercepts submits, builds `FormData`, sends an Async `fetch()`, and updates the DOM based on the JSON response.

### Key API Endpoints
All endpoints are registered in `WebServer.cpp`.

| Endpoint | Method | Payload | Description |
| :--- | :--- | :--- | :--- |
| **`/api/status`** | `GET` | JSON | Returns highly dynamic live states: Memory (`heap`), Storage (`fsUsed`), Wi-Fi (`rssi`), Time, and general device identifiers. Polled on load. |
| **`/api/live`** | `GET` | JSON | A compact JSON payload polled purely by the `#live` page every 500ms. Returns real-time flow `liters`, `pulses`, `cycleTime`, and state machine `state`. |
| **`/export_settings`**| `GET` | JSON | A massive JSON dump of the entire `ConfigManager` struct. Parsed by `web.js` on load to populate all inputs on the various Settings pages. |
| **`/save_*`** | `POST` | `application/x-www-form-urlencoded` | Multiple endpoints (e.g. `/save_network`, `/save_theme`) that accept form data, update `config`, call `ConfigManager.save()`, and return `{success:true}`. |
| **`/api/filelist`** | `GET` | JSON | Returns an array of files for the File Manager, including `name`, `path`, `isDir`, and `size`. Supports recursive filtering. |
| **`/do_update`** | `POST` | `multipart/form-data` | Accepts a compiled `firmware.bin` for OTA updates. Triggers ESP32 restart upon success. |

### Wi-Fi RSSI Logic Example
The network status logic demonstrates the backend/frontend split perfectly:
1. **Backend:** `/api/status` calls `WiFi.RSSI()` and attaches it to the JSON response (`{"rssi": -65}`).
2. **Frontend:** `web.js` receives `-65`, passes it to `getRssiInfo()`, which calculates the standard signal strength metric (3 out of 4 bars), selects the CSS mapped color (`text-primary`), and injects the dynamic SVG icon into the DOM alongside the text value.
