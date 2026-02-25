# Blueprint – Monolith (`full_logger.ino`)

> **Water Logger v4.1.4** · ESP32-C3 (XIAO) · Single-File Architecture  
> Generated: 2026-02-25

---

## Architecture Overview

```
full_logger.ino   (5341 lines, 286 KB)
 ├── [1–57]      File header / changelog comments
 ├── [58–320]    Constants, enums, structs (same as modular Config.h)
 ├── [321–408]   Global variables (same as modular Globals.cpp)
 ├── [410–448]   Forward declarations
 ├── [450–579]   Utility + ISR functions
 ├── [581–686]   NTP sync, bootcount backup/restore
 ├── [688–849]   Data logging (flush + addLogEntry)
 ├── [851–1123]  Configuration management (defaults, migrate, load, save)
 ├── [1125–1431] Storage, hardware, WiFi, wakeup functions
 ├── [1487–1680] JSON/HTML helpers (sendJsonResponse, sendRestartPage, sendChunkedHtml)
 ├── [1681–1873] File list, storage info, datalog helpers, RTC strings
 ├── [1875–4894] ★ setupWebServer() — ALL HTML pages + JS + API endpoints (3020 lines)
 ├── [4896–4918] deleteRecursive()
 ├── [4920–5117] setup()
 └── [5119–5341] loop()
```

> **Key difference from modular:** The monolith has **no external files** — all HTML, CSS, and JavaScript are embedded in C++ string literals inside `setupWebServer()`. The modular project extracts these to `www/index.html`, `www/web.js`, and `www/style.css`.

---

## 1. Type Definitions (Lines 58–320)

Identical to modular `Config.h` — same enums and structs:

| Type | Lines | Fields |
|------|-------|--------|
| `ThemeConfig` | 192–213 | mode, colors (11), logoSource, faviconPath, boardDiagramPath, chartSource, chartLocalPath, showIcons, chartLabelFormat |
| `DatalogConfig` | 221–242 | prefix, currentFile, folder, rotation, maxSizeKB, maxEntries, includeDeviceId, timestampFilename, date/time/end/volume formats, post-correction |
| `FlowMeterConfig` | 244–252 | pulsesPerLiter, calibrationMultiplier, monitoringWindowSecs, firstLoopMonitoringWindowSecs, testMode, blinkDuration |
| `HardwareConfig` | 254–274 | version, storageType, wakeupMode, all pin assignments, cpuFreqMHz, debugMode, defaultStorageView, debounceMs |
| `NetworkConfig` | 276–295 | wifiMode, AP/Client credentials, static IP, NTP, AP network settings, deviceId |
| `DeviceConfig` | 297–310 | magic, version, deviceId, deviceName, forceWebServer, resetBootCountAction, sub-configs |
| `LogEntry` | 312–320 | wakeTimestamp, sleepTimestamp, bootCount, ffCount, pfCount, volumeLiters, wakeupReason |

---

## 2. Global Variables (Lines 321–408)

Same as modular `Globals.cpp` — all variables defined at file scope.

---

## 3. All Functions

### Utility Functions (Lines 450–550)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `formatFileSize(bytes)` | 450–458 | `Utils.cpp` |
| `getVersionString()` | 460–465 | `Config.h` (inline) |
| `getModeDisplay()` | 467–472 | `WebServer.cpp` |
| `icon(emoji)` | 474–478 | ❌ **Monolith only** — returns emoji if showIcons |
| `getThemeClass()` | 480–486 | ❌ **Monolith only** — returns `"dark-theme"` or `""` |
| `getNetworkDisplay()` | 488–491 | `WebServer.cpp` |
| `generateDeviceId()` | 493–503 | `ConfigManager.cpp` |
| `regenerateDeviceId()` | 505–517 | `ConfigManager.cpp` |
| `getActiveDatalogFile()` | 519–527 | `StorageManager.cpp` |
| `buildPath(dir, name)` | 529–532 | `Utils.cpp` |
| `sanitizePath(path)` | 534–542 | `Utils.cpp` |
| `sanitizeFilename(filename)` | 544–550 | `Utils.cpp` |

### ISR Handlers (Lines 552–579)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `onFFButton()` | 552–561 | `HardwareManager.cpp` |
| `onPFButton()` | 563–569 | `HardwareManager.cpp` |
| `onFlowPulse()` | 571–579 | `HardwareManager.cpp` |
| `debounceButton(...)` | 581–599 | `HardwareManager.cpp` |

### NTP & Boot Count (Lines 601–686)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `syncTimeFromNTP()` | 601–651 | `WiFiManager.cpp` |
| `backupBootCount()` | 653–666 | `RtcManager.cpp` |
| `restoreBootCount()` | 668–686 | `RtcManager.cpp` |

### Data Logging (Lines 688–849)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `flushLogBufferToFS()` | 688–803 | `DataLogger.cpp` |
| `addLogEntry()` | 805–849 | `DataLogger.cpp` |

### Configuration (Lines 851–1123)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `loadDefaultConfig()` | 851–957 | `ConfigManager.cpp` |
| `migrateConfig(fromVersion)` | 959–979 | `ConfigManager.cpp` |
| `loadConfig()` | 981–1107 | `ConfigManager.cpp` |
| `saveConfig()` | 1109–1123 | `ConfigManager.cpp` |

### Storage & Hardware (Lines 1125–1431)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `initStorage()` | 1125–1172 | `StorageManager.cpp` |
| `initHardware()` | 1174–1325 | `HardwareManager.cpp` |
| `connectToWiFi()` | 1327–1374 | `WiFiManager.cpp` |
| `startAPMode()` | 1376–1396 | `WiFiManager.cpp` |
| `safeWiFiShutdown()` | 1398–1414 | `WiFiManager.cpp` |
| `configureWakeup()` | 1416–1431 | `RtcManager.cpp` |
| `getWakeupReason()` | 1433–1479 | `RtcManager.cpp` |

### JSON/HTML Helpers (Lines 1487–1680)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `sendJsonResponse(r, doc)` | 1487–1494 | `WebServer.cpp` |
| `sendRestartPage(r, msg)` | 1496–1512 | `WebServer.cpp` |
| `getCurrentPage(title)` | 1514–1524 | ❌ **Monolith only** — multi-page nav highlighting |
| `writeSidebar(out, page)` | 1526–1562 | ❌ **Monolith only** — desktop sidebar HTML |
| `writeBottomNav(out, page)` | 1564–1589 | ❌ **Monolith only** — mobile bottom nav HTML |
| `sendChunkedHtml(r, title, bodyWriter)` | 1591–1680 | ❌ **Monolith only** — chunked HTML page template |

### File/Storage Helpers (Lines 1681–1873)

| Function | Lines | Modular Equivalent |
|----------|-------|---------------------|
| `getCurrentViewFS()` | 1686–1694 | `StorageManager.cpp` |
| `writeFileList(out, dir, editMode)` | 1696–1752 | ❌ **Monolith only** — server-side HTML file list |
| `getStorageInfo(used, total, pct, type)` | 1754–1776 | `StorageManager.cpp` |
| `getStorageBarColor(pct)` | 1778–1782 | `StorageManager.cpp` |
| `generateDatalogFileOptions()` | 1784–1816 | `StorageManager.cpp` |
| `countDatalogFiles()` | 1818–1842 | `StorageManager.cpp` |
| `getRtcTimeString()` | 1844–1859 | `RtcManager.cpp` |
| `getRtcDateTimeString()` | 1861–1873 | `RtcManager.cpp` |
| `deleteRecursive(fs, path)` | 4896–4918 | `Utils.cpp` |

---

## 4. `setupWebServer()` (Lines 1875–4894) — ★ 3020 Lines

This is the **heart of the monolith**. It registers all HTTP endpoints and includes **all embedded HTML pages with inline CSS and JavaScript**.

### Architecture: Server-Side Rendering

Unlike the modular SPA, the monolith uses **server-side rendered multi-page** architecture:
- Each page is rendered via `sendChunkedHtml()` with a lambda that writes HTML to a `Print&` stream
- JavaScript is embedded inline within each page's `<script>` tags
- CSS is partly inline, partly served from `/style.css` on LittleFS
- Navigation uses `writeSidebar()` + `writeBottomNav()` for consistent chrome

### API & Page Endpoints

#### HTML Pages (Server-Side Rendered)

| Method | Path | Lines | Description |
|--------|------|-------|-------------|
| GET | `/` | ~1900–2200 | Dashboard page — status bar + chart + data table |
| GET | `/data` | ~2200–2500 | Data view — filtered log data + chart |
| GET | `/settings` | ~2500–3200 | Settings page — device, flow, hardware, theme, network, time, datalog tabs |
| GET | `/files` | ~3200–3700 | File manager — upload, browse, delete, move/rename |
| GET | `/live` | ~3700–4000 | Live monitor — real-time sensor readings + state machine |

#### JSON API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Runtime status JSON (same as modular) |
| GET | `/api/config` | Full config export |
| GET | `/api/files?dir=/` | JSON file listing |
| GET | `/api/live` | Live sensor data (polled by inline JS) |
| GET | `/api/recent_logs` | Last N log entries |
| GET | `/api/changelog` | Serve changelog.txt |

#### Save Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/save_hardware` | Save HW config → restart |
| POST | `/save_network` | Save network config → restart |
| POST | `/save_datalog` | Save datalog format + optional file create |
| POST | `/save_flowmeter` | Save flow meter settings |
| POST | `/save_theme` | Save theme colors |
| POST | `/save_device` | Save device name/ID |
| POST | `/save_time` | Save NTP/timezone |

#### Action Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/set_time` | Manual RTC time set |
| POST | `/sync_ntp` | NTP sync |
| POST | `/rtc_protect` | RTC write protection |
| POST | `/flush_logs` | Flush log buffer |
| POST | `/backup_bootcount` | Backup boot counter |
| POST | `/restore_bootcount` | Restore boot counter |
| POST | `/restart` | Restart device |
| POST | `/factory_reset` | Format LittleFS + restart |

#### File Operations

| Method | Path | Description |
|--------|------|-------------|
| POST | `/upload?dir=/www/` | Upload file |
| GET | `/download?path=...` | Download file |
| GET | `/delete?path=...` | Delete file/folder |
| POST | `/mkdir?dir=...` | Create directory |
| GET | `/move_file` | Move/rename file |

#### WiFi Scan

| Method | Path | Description |
|--------|------|-------------|
| GET | `/wifi_scan_start` | Start async scan |
| GET | `/wifi_scan_result` | Get results |

#### OTA

| Method | Path | Description |
|--------|------|-------------|
| POST | `/do_update` | OTA firmware upload |

#### Settings Export/Import

| Method | Path | Description |
|--------|------|-------------|
| GET | `/export_settings` | Download config JSON |
| POST | `/import_settings` | Upload config JSON |

### Embedded JavaScript Functions (inside `setupWebServer()`)

Each HTML page contains inline `<script>` blocks. Key JS functions per page:

| Page | JS Functions | Description |
|------|-------------|-------------|
| **Dashboard** | `loadData()`, `processData(data)`, `renderChart(data)`, `applyFilters()`, `exportCSV()` | Chart visualization with filters |
| **Live** | `upd()`, `updLogs()`, `btn(id,pressed,...)` | 500ms/3s polling loops |
| **Files** | `loadFiles()`, `uploadFile()`, `deleteFile(path)`, `mkdir()`, `showMovePopup()`, `applyMove()` | File manager actions |
| **Settings** | `scanWifi()`, `checkScanResult()`, `toggleMode()`, `toggleStatic()`, `updatePreview()`, `toggleManualId(id)` | Settings-specific interactions |

---

## 5. `setup()` (Lines 4920–5117)

Same sequence as modular, but all function calls are within the same file:

```
1. Early GPIO capture (bitmask snapshot before any pin init)
2. Serial.begin() + banner print
3. initStorage()
4. loadConfig() + migrateConfig()
5. Wakeup reason detection + button debounce
6. initHardware()
7. WiFi connect / AP mode decision
8. setupWebServer()
9. State machine initialization
```

## 6. `loop()` (Lines 5119–5341)

Identical logic to modular `Logger.ino::loop()`:

```
1. debounceButton() for FF and PF pins
2. State machine: IDLE → WAIT_FLOW → MONITORING → DONE
3. Flow sensor monitoring + volume calculation
4. addLogEntry() + flushLogBufferToFS()
5. configureWakeup() + esp_deep_sleep_start()
6. Deferred restart handling
```

---

## Monolith-Only Functions (Not in Modular)

These functions exist **only** in `full_logger.ino` — they handle server-side HTML rendering which the modular version delegates to `www/index.html` + `web.js`:

| Function | Lines | Purpose |
|----------|-------|---------|
| `icon(emoji)` | 474–478 | Return emoji string if `showIcons` enabled |
| `getThemeClass()` | 480–486 | Return `"dark-theme"` CSS class for server-side rendering |
| `getCurrentPage(title)` | 1514–1524 | Determine which nav item is active |
| `writeSidebar(out, page)` | 1526–1562 | Render desktop sidebar nav HTML |
| `writeBottomNav(out, page)` | 1564–1589 | Render mobile bottom nav HTML |
| `sendChunkedHtml(r, title, bodyWriter)` | 1591–1680 | Full page HTML template (head, theme CSS vars, body chrome) |
| `writeFileList(out, dir, editMode)` | 1696–1752 | Server-side HTML file listing |

---

## Modular ↔ Monolith Comparison

| Aspect | Monolith | Modular |
|--------|----------|---------|
| **Files** | 1 file (5341 lines) | 17+ files |
| **Flash** | ~286 KB source | ~81 KB C++ + ~169 KB frontend |
| **UI Architecture** | Server-side rendered multi-page | Client-side SPA (single HTML + JS) |
| **HTML/CSS/JS** | Embedded in C++ PROGMEM strings | External files in `/www/` |
| **Page Navigation** | Full page reload | Hash-based client routing |
| **Theme Application** | Server-side CSS class injection | Client-side CSS variable update |
| **API Surface** | Same endpoints | Same endpoints |
| **Failsafe Mode** | N/A (HTML always in firmware) | Embedded minimal upload page |
| **Update UI** | Requires firmware reflash | Upload files to `/www/` |
| **Version** | v4.1.4 | v4.1.5 |
