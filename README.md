# 💧 ESP32 Low-Power Water Usage Logger

Low-power multi-sensor water usage logger built for **XIAO ESP32-C3 (RISC-V)** with deep sleep wake-up, web interface, file management, configurable datalogging and advanced button post-correction logic.

---

## 📌 Project Overview

**Project:** ESP32 Low-Power Water Usage Logger  
**Version:** 4.1.4  
**Target Board:** Seeed Studio XIAO ESP32-C3  
**Author:** Petko Georgiev  
**Organization:** Villeroy & Boch Bulgaria  
**Location:** Sevlievo, Bulgaria  

This firmware is designed to:

- Log water usage via flow sensor
- Detect flush plate buttons (PF / FF)
- Operate in ultra low-power deep sleep mode
- Provide web-based configuration and monitoring
- Support LittleFS or SD storage
- Automatically correct misidentified flush events using volume logic

<details>
 <summary>⚙️ Core Features</summary>
 
### 🔋 Low Power Operation
- Deep sleep using GPIO wake-up
- Automatic sleep after inactivity
- Optimized boot sequence for fast button detection (<1ms snapshot)

### 🚽 Multi-Button Detection
- Supports:
  - PF (Partial Flush)
  - FF (Full Flush)
- Configurable debounce (20–500ms)
- Edge detection to prevent multiple counts
- Early GPIO snapshot to avoid misidentification

### 🧠 Post-Correction Logic (v4.1.4+)
Volume-based correction of button identification:

- PF with volume ≥ threshold → corrected to FF
- FF with volume ≤ threshold → corrected to PF
- Configurable thresholds:
  - `pfToFfThreshold`
  - `ffToPfThreshold`
- Manual press detection:
  - If button held longer than `manualPressThresholdMs` (default 500ms), correction is skipped

Post-correction events are logged to: /btn_log.txt

</details>
<details>
 <summary>🌐 Web Interface</summary>

Accessible in:

- 🔵 AP Mode (default)
- 🟢 Client Mode (WiFi network)
- 🔴 Logging Mode (offline)

### Web Features

- Dashboard with:
  - Button-only filtering
  - Exclude 0.00L option
  - CSV export
- Settings pages:
  - Device
  - Hardware
  - Flow Meter
  - Datalog
  - Network
- File Manager:
  - Upload
  - Move / Rename
  - Folder selection
- Import / Export configuration
- Restart confirmation popup
- Live status JSON endpoints
- Integrated changelog viewer (reads `/changelog.txt`)

</details>
<details>
 <summary>💾 Storage Support</summary>

Selectable storage type:

- LittleFS (default)
- SD Card

### Datalog Features

- Configurable file rotation
- Max size / entry limits
- Filename timestamp option
- Device ID in logs
- Wake time AND sleep time included
- Flexible formatting:
  - Date format
  - Time format
  - End format (duration / time)
  - Volume format

</details>
<details>
 <summary>🔧 Hardware Configuration (Default Pins – XIAO ESP32-C3)</summary>

| Function        | Default Pin |
|---------------|------------|
| WiFi Trigger   | D0 (GPIO 2) |
| Wakeup FF      | D1 (GPIO 3) |
| Wakeup PF      | D2 (GPIO 4) |
| Flow Sensor    | D6 (GPIO 21) |
| RTC (DS1302)   | 5 / 6 / 7 |
| SD Card        | 10–13 |

CPU default frequency: **80 MHz**
</details>
<details>
 <summary>🌍 Network Configuration</summary>

### Default AP Mode

- SSID: `WaterLogger`
- Password: `water12345`
- Default IP: `192.168.4.1`

### Client Mode

- DHCP or Static IP supported
- Configurable:
  - IP
  - Gateway
  - Subnet
  - DNS
- NTP Server default: pool.ntp.org
- Default timezone: EET (UTC+2)
</details>
<details>
 <summary>📊 API Endpoints</summary>

| Endpoint | Description |
|----------|------------|
| `/api/status` | Live device status (JSON) |
| `/api/recent_logs` | Last 5 log entries |
| `/api/changelog` | Loads changelog from LittleFS |
| `/api/regen-id` | Generate new device ID from MAC |

</details>
<details>
 <summary>🆔 Device ID</summary>

- Automatically generated from ESP32 MAC address
- Uses last 4 bytes of MAC
- Can be manually edited
- Regeneration available via API

</details>
<details>
 <summary>🧪 Test Mode</summary>

- WiFi pin used as LED output
- Visual flow validation
- Adjustable blink duration

</details>
<details>
 <summary>🛠 Technical Stack</summary>

- Arduino Framework
- ESPAsyncWebServer
- AsyncTCP
- LittleFS
- SD
- DS1302 RTC
- ArduinoJson
- ESP32 Deep Sleep API
- FlowSensor library

</details>
<details>
 <summary>📦 Version History (Latest)</summary>

### v4.1.4 – Post-Correction Improvements

- Button hold duration measurement
- Extended hold skips correction
- `manualPressThresholdMs` setting (default 500ms)
- Cleaner UI when post-correction disabled
- Config migration handling fix
- Standardized version format

### v4.1.3

- Early GPIO snapshot (<1ms)
- Volume-based correction logic introduced
- Post-correction logging
- CONFIG_VERSION 10

### v4.1.2

- Wake-up reliability fix
- CSS moved to external file (~30KB flash saved)

### v4.1.1

- File Manager improvements
- UI consistency fixes
- Code size optimization (~5KB saved)

### v4.1.0 – Major Release

- Removed multi-language (English only)
- CSV export
- Import/Export settings
- Manual Device ID editing
- Restart confirmation popup
- Wake & sleep timestamps in datalog
</details>
<details>
 <summary>🚀 Flashing</summary>

1. Select board: XIAO ESP32-C3
2. Set CPU frequency to 160 MHz
3. Upload filesystem (LittleFS) including:
- `/changelog.txt`
- /styles.css
- Static assets /www/
</details>
<details>
 <summary>🔒 Debug Mode</summary>

#define DEBUG_MODE 0
Set to 1 to enable Serial debug output
Saves ~3KB flash when disabled.
</details>

## 🧩 Project Goals

Reliable water usage tracking

Minimal false button identification

Ultra-low power consumption

Professional embedded UI

Easy configuration & deployment

## 📄 License

Internal / Custom project (define license if needed).

## 👨‍💻 Author

Petko Georgiev
Villeroy & Boch Bulgaria
Sevlievo, Bulgaria

