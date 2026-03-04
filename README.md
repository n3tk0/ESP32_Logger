<div align="center">
  <h1>💧 ESP32 Water Logger</h1>
  <p><b>Low-power, modular multi-sensor water usage logger for Seeed XIAO ESP32-C3</b></p>
  
  [![ESP32-C3](https://img.shields.io/badge/Board-Seeed_XIAO_ESP32--C3-blue)](#)
  [![C++](https://img.shields.io/badge/Language-C++-00599C?logo=c%2B%2B)](#)
  [![SPA](https://img.shields.io/badge/Frontend-Vanilla_JS_SPA-F7DF1E)](#)
  [![LittleFS](https://img.shields.io/badge/Filesystem-LittleFS-darkgreen)](#)
</div>

---

## 📌 Project Overview

**ESP32 Water Logger** is a highly optimized, low-power logging system designed to track water usage exactly when it happens. By heavily utilizing hardware interrupts, deep sleep, and a modular C++ architecture, the device consumes minimal power while guaranteeing accurate flow monitoring.

The system features a rich, responsive **Single Page Application (SPA)** served directly from the ESP32's LittleFS. 

This project recently underwent a major architectural migration (v4.1.x) from a monolithic `.ino` design to a highly scalable, modular codebase cleanly separating hardware logic, API endpoints, and frontend assets.

- **Author:** Petko Georgiev

---

## ⚙️ Key Features

### 🔋 Ultra-Low Power & Reliability
- Relies on **Deep Sleep** loops, waking only via GPIO interrupts (Flush Buttons or Flow Sensor).
- Safe Restart paths with clean Wi-Fi hardware shutdown (`safeWiFiShutdown()`).

### 🌊 Accurate Flow Detection & Post-Correction
- Supports independent triggers: **Full Flush (FF)** and **Part Flush (PF)**.
- **Smart Post-Correction:** Automatically corrects a logged PF to FF (or vice versa) if the monitored volume crosses configurable threshold parameters in the settings.
- Bypasses correction if the user intentionally holds the button (`manualPressThresholdMs`).

### 📱 Modern Modular Web Interface
- The UI is a pure **Vanilla JS Single Page Application (SPA)** loaded tightly from `/www/`.
- Features a highly responsive grid layout and custom **Theming System** (Light/Dark/Auto) governed by CSS Variables seamlessly injected via the backend API.
- **Dynamic Network Feedback:** Real-time visual Wi-Fi Signal Strength (RSSI) indicators using SVG icons mapped dynamically to dBm thresholds and theme colors.

### 💾 Flexible Storage & Datalogging
- Logs directly to **LittleFS** (Internal) or **SD Card** (SPI).
- Advanced automated log rotation (Daily, Weekly, Monthly, or By Size).
- Exports data cleanly to CSV right from the browser.

### 🛟 Resilient Failsafe Recovery
- If the primary SPA files (`/www/index.html`) ever become corrupted or deleted, the backend automatically intercepts routing and serves a hardcoded "Failsafe UI".
- The immutable `/setup` route is always available to easily drag-and-drop recovery files without serial flashing.
- Robust OTA firmware updating supporting `.bin` magic byte `0xE9` validation.

---

## 🔌 Hardware Requirements & Default Pins

Designed optimally for the **Seeed Studio XIAO ESP32-C3 (RISC-V)**.

| Component      | Default Pin | Description |
|---------------|-------------|-------------|
| **WiFi Trigger**| D0 (GPIO 2) | Wakes device to force AP/Web Server on. |
| **Wakeup FF** | D1 (GPIO 3) | Full Flush Trigger. |
| **Wakeup PF** | D2 (GPIO 4) | Part Flush Trigger. |
| **Flow Sensor**| D6 (GPIO 21)| Interfaced with the water flow meter. |
| **RTC (DS1302)**| D3/D4/D5 | Real-Time Clock (CE, IO, SCLK). |
| **SD Card SPI**| D7/D8/D9/D10 | Configurable via Hardware Settings. |

---

## 🚀 Software & Build Setup

### 1. Compile and Flash the Backend
Compile the C++ source code via Arduino IDE or PlatformIO. Ensure your partition scheme allocates enough room for **LittleFS** (e.g., Minimum 1MB or 2MB APP / 2MB FATFS).
- Flash the compiled firmware directly via USB.

### 2. Deploy the Frontend (LittleFS)
The Web UI does not live in C++ strings. It must be uploaded to the LittleFS partition.
1. Connect to the ESP32's Access Point (usually `WaterLogger`) or its designated IP.
2. The firmware will likely serve the **Recovery Page** (since `/www/index.html` is missing).
3. Using the recovery page, upload the following core files into the root or `/www/` directory:
   - `www/index.html`
   - `www/web.js`
   - `www/style.css`
   - `www/changelog.txt` 
4. Once successfully uploaded, the ESP32 will immediately route users to the beautiful SPA Dashboard.

---

## 📜 Migration Note

The transition to the **Modular Architecture** separates the monolithic `full_logger.ino` into clean, domain-driven Manager classes (`ConfigManager`, `HardwareManager`, `WiFiManager`, etc.) located in `src/`. 

For an in-depth reading of the internal modules, file tree, request loops, and JSON payload structures, please refer to the dedicated [`ARCHITECTURE.md`](ARCHITECTURE.md) document.

---

## 🛠 Tech Stack

- **Firmware:** Arduino Framework (ESP32-C3 / RISC-V)
- **Web Stack:** ESPAsyncWebServer + AsyncTCP, ArduinoJson
- **Frontend Core:** HTML5, Vanilla JavaScript, CSS3
- **Hardware Libs:** LittleFS, SD, RtcDS1302 (Makuna), FlowSensor

---

*Internal / Custom Project.*
