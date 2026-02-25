# Blueprint – Modular Project

> **Water Logger v4.1.5** · ESP32-C3 (XIAO) · Modular Architecture  
> Generated: 2026-02-25

---

## Architecture Overview

```
Logger.ino                          ← Orchestrator (setup/loop)
├── src/core/
│   ├── Config.h                    ← Structs, enums, constants (single source of truth)
│   ├── Globals.h                   ← Extern declarations for all global state
│   └── Globals.cpp                 ← Global variable definitions
├── src/managers/
│   ├── ConfigManager.h / .cpp      ← Config load/save/migrate/defaults
│   ├── DataLogger.h / .cpp         ← Log entry creation & flush to FS
│   ├── HardwareManager.h / .cpp    ← GPIO init, ISRs, button debounce
│   ├── RtcManager.h / .cpp         ← DS1302 RTC, bootcount backup, wakeup
│   ├── StorageManager.h / .cpp     ← LittleFS/SD init, storage info, datalog helpers
│   └── WiFiManager.h / .cpp        ← WiFi connect/AP, NTP sync, safe shutdown
├── src/utils/
│   └── Utils.h / .cpp              ← String/path/FS helpers
├── src/web/
│   ├── WebServer.h                 ← Public API (setupWebServer, helpers)
│   └── WebServer.cpp               ← All HTTP routes, failsafe HTML, JSON API
└── www/                            ← Frontend (served from LittleFS /www/)
    ├── index.html                  ← SPA shell (96 KB)
    ├── web.js                      ← SPA JavaScript (61 KB, 94 functions)
    ├── style.css                   ← Stylesheet (11 KB)
    └── chart.min.js                ← Chart.js library (205 KB)
```

---

## 1. Core Layer (`src/core/`)

### Config.h (241 lines)
Single source of truth for version, constants, enumerations, and all config structs.

| Item | Type | Description |
|------|------|-------------|
| `VERSION_MAJOR/MINOR/PATCH` | `#define` | Firmware version (4.1.5) |
| `getVersionString()` | `inline function` | Returns `"v4.1.5"` string |
| `DEBUG_MODE`, `DBG/DBGLN/DBGF` | Macros | Compile-time debug toggle |
| `CONFIG_FILE` | `constexpr` | `"/config.bin"` |
| `BOOTCOUNT_BACKUP_FILE` | `constexpr` | `"/bootcount.bin"` |
| `DEFAULT_AP_SSID` | `constexpr` | `"WaterLogger"` |
| `DEFAULT_AP_PASSWORD` | `constexpr` | `"water12345"` |
| `DEFAULT_NTP_SERVER` | `constexpr` | `"pool.ntp.org"` |
| `DefaultPins::*` | `namespace` | XIAO ESP32-C3 pin defaults |
| `StorageType` | `enum` | `STORAGE_LITTLEFS`, `STORAGE_SD_CARD` |
| `WiFiModeType` | `enum` | `WIFIMODE_AP`, `WIFIMODE_CLIENT` |
| `ThemeMode` | `enum` | `THEME_LIGHT`, `THEME_DARK`, `THEME_AUTO` |
| `ChartSource` | `enum` | `CHART_LOCAL`, `CHART_CDN` |
| `WakeupMode` | `enum` | `WAKEUP_GPIO_ACTIVE_HIGH/LOW` |
| `DatalogRotation` | `enum` | `ROTATION_NONE/DAILY/WEEKLY/MONTHLY/SIZE` |
| `ChartLabelFormat` | `enum` | `LABEL_DATETIME/BOOTCOUNT/BOTH` |
| `DateFormat` | `enum` | `DATE_OFF/DDMMYYYY/MMDDYYYY/YYYYMMDD/DDMMYYYY_DOT` |
| `TimeFormat` | `enum` | `TIME_HHMMSS/HHMM/12H` |
| `EndFormat` | `enum` | `END_TIME/DURATION/OFF` |
| `VolumeFormat` | `enum` | `VOL_L_COMMA/L_DOT/NUM_ONLY/OFF` |
| `LoggingState` | `enum` | `STATE_IDLE/WAIT_FLOW/MONITORING/DONE` |
| `ThemeConfig` | `struct` | Theme colors, paths, chart settings |
| `DatalogConfig` | `struct` | Prefix, folder, rotation, format options, post-correction |
| `FlowMeterConfig` | `struct` | Pulses/L, calibration, monitoring windows, test mode |
| `HardwareConfig` | `struct` | Pin assignments, storage type, CPU freq, debounce |
| `NetworkConfig` | `struct` | WiFi mode, AP/Client credentials, static IP, NTP |
| `DeviceConfig` | `struct` | Top-level config wrapping all above + deviceId/Name |
| `LogEntry` | `struct` | Wake/sleep timestamps, boot count, FF/PF counts, volume |

### Globals.h / Globals.cpp (106 + 98 lines)
All global state variables (extern declarations + definitions).

| Category | Variables |
|----------|-----------|
| **Objects** | `config`, `rtcWire`, `Rtc`, `Sensor`, `server(80)` |
| **Storage** | `activeFS`, `sdAvailable`, `littleFsAvailable`, `fsAvailable`, `currentStorageView` |
| **WiFi** | `apModeTriggered`, `wifiConnectedAsClient`, `wifiFallbackToAP`, `onlineLoggerMode`, `currentIPAddress`, `connectedSSID` |
| **Logging Buffer** | `logBuffer[20]` _(RTC_DATA_ATTR)_, `logBufferCount`, `bootCount`, `bootcount_restore` |
| **Wake/Cycle** | `currentWakeTimestamp`, `wakeUpButtonStr`, `cycleStartedBy`, `cycleButtonSet`, `cycleStartTime`, `cycleTotalPulses` |
| **Early GPIO** | `earlyGPIO_bitmask`, `earlyGPIO_captured`, `earlyGPIO_millis`, `buttonHeldMs` |
| **Debounce** | `highCountFF/PF`, `stableFF/PFState`, `lastFF/PFDebounceTime`, `lastFF/PFButtonState` |
| **ISR** | `pulseCount`, `lastFF/PF/FlowInterrupt`, `ffPressed`, `pfPressed`, `flowSensorPulseDetected`, `isrDebounceUs` |
| **State Machine** | `loggingState`, `stateStartTime`, `lastFlowPulseTime` |
| **System** | `rtcValid`, `shouldRestart`, `restartTimer`, `statusMessage`, `currentDir` |

---

## 2. Manager Layer (`src/managers/`)

### ConfigManager (413 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `applyDefaults` | `void applyDefaults()` | _(internal)_ Apply safe defaults to zero/empty fields |
| `sanitizeWakeConfig` | `bool sanitizeWakeConfig()` | Validate wake pin configuration |
| `generateDeviceId` | `String generateDeviceId()` | Generate device ID from MAC address |
| `regenerateDeviceId` | `void regenerateDeviceId()` | Regenerate and save new device ID |
| `loadDefaultConfig` | `void loadDefaultConfig()` | Initialize config struct with factory defaults |
| `migrateConfig` | `void migrateConfig(uint8_t fromVersion)` | Migrate config from older version |
| `loadConfig` | `bool loadConfig()` | Load config from `/config.bin` with migration |
| `saveConfig` | `bool saveConfig()` | Persist config to `/config.bin` |

### DataLogger (158 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `flushLogBufferToFS` | `void flushLogBufferToFS()` | Write buffered log entries to datalog file |
| `addLogEntry` | `void addLogEntry()` | Create new entry in log buffer |

### HardwareManager (80 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `onFFButton` | `void IRAM_ATTR onFFButton()` | ISR: Full-flush button interrupt |
| `onPFButton` | `void IRAM_ATTR onPFButton()` | ISR: Partial-flush button interrupt |
| `onFlowPulse` | `void IRAM_ATTR onFlowPulse()` | ISR: Flow sensor pulse interrupt |
| `debounceButton` | `void debounceButton(...)` | Polling-based button debounce |
| `initHardware` | `void initHardware()` | Initialize GPIO pins + RTC |

### RtcManager (198 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `initRtc` | `void initRtc()` | Initialize DS1302 RTC with validation |
| `backupBootCount` | `void backupBootCount()` | Save bootcount to RTC RAM + LittleFS |
| `restoreBootCount` | `void restoreBootCount()` | Restore bootcount from RTC RAM or flash |
| `getRtcTimeString` | `String getRtcTimeString()` | Format: `HH:MM:SS` |
| `getRtcDateTimeString` | `String getRtcDateTimeString()` | Format: `YYYY-MM-DD HH:MM:SS` |
| `configureWakeup` | `void configureWakeup()` | Setup ESP32-C3 deep sleep GPIO wakeup mask |
| `getWakeupReason` | `String getWakeupReason()` | Determine which button caused wake-up |

### StorageManager (132 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `initStorage` | `bool initStorage()` | Initialize LittleFS + optional SD card |
| `getCurrentViewFS` | `fs::FS* getCurrentViewFS()` | Get filesystem for current browser view |
| `getActiveDatalogFile` | `String getActiveDatalogFile()` | Build active datalog file path |
| `getStorageInfo` | `void getStorageInfo(...)` | Get used/total/percent for a storage type |
| `getStorageBarColor` | `String getStorageBarColor(int pct)` | Color based on percentage (green/yellow/red) |
| `generateDatalogFileOptions` | `String generateDatalogFileOptions()` | HTML `<option>` list of log files |
| `countDatalogFiles` | `int countDatalogFiles()` | Count .txt/.log/.csv files recursively |

### WiFiManager (139 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `safeWiFiShutdown` | `void safeWiFiShutdown()` | Clean WiFi state before restart |
| `connectToWiFi` | `bool connectToWiFi()` | Connect as client with optional static IP |
| `startAPMode` | `void startAPMode()` | Start SoftAP with configured SSID/password |
| `syncTimeFromNTP` | `bool syncTimeFromNTP()` | Sync RTC from NTP server |

---

## 3. Utility Layer (`src/utils/`)

### Utils (51 lines)

| Function | Signature | Description |
|----------|-----------|-------------|
| `formatFileSize` | `String formatFileSize(uint64_t bytes)` | Human-readable size (B/KB/MB/GB) |
| `buildPath` | `String buildPath(dir, name)` | Combine directory + filename |
| `sanitizePath` | `String sanitizePath(path)` | Remove `..`, `//`, ensure leading `/` |
| `sanitizeFilename` | `String sanitizeFilename(filename)` | Remove `..` and `//` |
| `deleteRecursive` | `bool deleteRecursive(fs, path)` | Recursively delete directory |

---

## 4. Web Server Layer (`src/web/WebServer.cpp`, 1438 lines)

### Exported Helper Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `getModeDisplay` | `String getModeDisplay()` | `"Online Logger"` / `"Web Server"` / `"Logger"` |
| `getNetworkDisplay` | `String getNetworkDisplay()` | Connected SSID or AP name |
| `sendJsonResponse` | `void sendJsonResponse(r, doc)` | Serialize and send JSON |
| `sendRestartPage` | `void sendRestartPage(r, msg)` | HTML restart countdown page |

### Internal Helpers

| Function | Scope | Description |
|----------|-------|-------------|
| `getMime(path)` | `static` | MIME type by file extension |
| `scanDir(fs,dir,arr,filter,recursive)` | `static` | Populate JSON array with directory listing |
| `fmtIP(ip,buf16)` | `static` | `uint8_t[4]` → `"A.B.C.D"` |

### HTTP Endpoints (API Map)

#### Static / SPA Routes

| Method | Path | Handler | Description |
|--------|------|---------|-------------|
| GET | `/` | LittleFS `/www/index.html` or failsafe | Main SPA entry |
| GET | `/setup` | failsafe HTML | Always-available recovery page |
| GET | `/dashboard`, `/files`, `/live`, `/settings`, `/settings_*` | redirect → `/` | SPA client-side routing |

#### JSON API Endpoints

| Method | Path | Response Keys | Description |
|--------|------|---------------|-------------|
| GET | `/api/status` | `device`, `deviceId`, `version`, `time`, `network`, `ip`, `boot`, `heap`, `heapTotal`, `heapPct`, `chip`, `cpu`, `mode`, `wifi`, `freeSketch`, `fsUsed`, `fsTotal`, `fsPct`, `defaultStorageView`, `rtcProtected`, `rtcRunning`, `theme{...}` | Full runtime status + theme |
| GET | `/api/live` | `time`, `ff`, `pf`, `wifi`, `pulses`, `boot`, `heap`, `heapTotal`, `uptime`, `trigger`, `cycleTime`, `ffCount`, `pfCount`, `totalPulses`, `state`, `stateTime`, `stateRemaining`, `liters`, `mode`, `fsUsed`, `fsTotal`, `ip` | Live sensor polling (500ms) |
| GET | `/api/recent_logs` | `logs[]` → `{time, trigger, volume, ff, pf}` | Last 5 log entries parsed |
| GET | `/api/filelist` | `files[]` → `{name, path, isDir, size}`, `used`, `total`, `percent`, `currentFile` | Directory listing with storage info |
| GET | `/api/changelog` | plain text | Serve changelog.txt from `/www/` or `/` |
| POST | `/api/regen-id` | plain text (new ID) | Generate new device ID from MAC |

#### Settings Export/Import

| Method | Path | Description |
|--------|------|-------------|
| GET | `/export_settings` | Full config JSON download (all sections) |
| POST | `/import_settings` | Upload JSON to restore config |

#### Save Endpoints

| Method | Path | Params | Action |
|--------|------|--------|--------|
| POST | `/save_device` | `deviceName`, `deviceId`, `forceWebServer`, `defaultStorageView` | Save + 200 OK |
| POST | `/save_flowmeter` | `pulsesPerLiter`, `calibrationMultiplier`, `monitoringWindowSecs`, `firstLoopWindowSecs`, `testMode`, `blinkDuration`, `resetBootCount` | Save + 200 OK |
| POST | `/save_hardware` | `storageType`, `wakeupMode`, `pin*`, `cpuFreqMHz`, `debounceMs` | Save + **restart** |
| POST | `/save_theme` | `themeMode`, `showIcons`, `*Color`, `logoSource`, `faviconPath`, `boardDiagramPath`, `chartSource`, `chartLocalPath`, `chartLabelFormat` | Save + 200 OK |
| POST | `/save_datalog` | `currentFile`, `prefix`, `folder`, `rotation`, `maxSizeKB`, `dateFormat`, `timeFormat`, `endFormat`, `volumeFormat`, `includeBootCount`, `includeExtraPresses`, `postCorrectionEnabled`, `pfToFfThreshold`, `ffToPfThreshold`, `manualPressThresholdMs`, `action` | Save + optional file create |
| POST | `/save_network` | `wifiMode`, `apSSID`, `apPassword`, `clientSSID`, `clientPassword`, `useStaticIP`, `staticIP`, `gateway`, `subnet`, `dns`, `apIP`, `apGateway`, `apSubnet` | Save + **restart** |
| POST | `/save_time` | `ntpServer`, `timezone` | Save + 200 OK |

#### Time Management

| Method | Path | Description |
|--------|------|-------------|
| POST | `/set_time` | Manual RTC set (`date` + `time` params) |
| POST | `/sync_time` | NTP sync |
| POST | `/rtc_protect` | Enable/disable RTC write protection |
| POST | `/flush_logs` | Force flush log buffer to filesystem |
| POST | `/backup_bootcount` | Backup boot counter |
| POST | `/restore_bootcount` | Restore boot counter |

#### File Operations

| Method | Path | Params | Description |
|--------|------|--------|-------------|
| GET | `/download` | `file`, `storage` | Download file as attachment |
| GET | `/delete` | `path`, `storage` | Delete file or directory (recursive) |
| GET | `/mkdir` | `name`, `dir`, `storage` | Create directory |
| GET | `/move_file` | `src`, `newName`, `destDir`, `storage` | Move/rename file |
| POST | `/upload` | multipart `file` + `path`, `storage` | Upload file (default to `/www/`) |

#### System

| Method | Path | Description |
|--------|------|-------------|
| GET/POST | `/restart` | Deferred restart via `shouldRestart` flag |
| POST | `/factory_reset` | Format LittleFS + restart |

#### WiFi Scan

| Method | Path | Description |
|--------|------|-------------|
| GET | `/wifi_scan_start` | Start async WiFi scan |
| GET | `/wifi_scan_result` | Get scan results JSON |

#### OTA Update

| Method | Path | Description |
|--------|------|-------------|
| POST | `/do_update` | Upload `.bin` firmware OTA |

---

## 5. Frontend SPA (`www/web.js`, 1292 lines)

### Bootstrap & Navigation

| Function | Line | Description |
|----------|------|-------------|
| _(DOMContentLoaded)_ | 34 | Fetches `/api/status` + `/export_settings`, applies theme, routes to hash |
| _(hashchange)_ | 49 | Re-routes on URL hash change |
| `applyStatus(d)` | 54 | Apply status data: theme colors, footer, header |
| `updateFooter(d)` | 129 | Partial footer update (time, heap, boot) |
| `nav(el)` | 142 | Navigation click handler |
| `navigateTo(page)` | 151 | Show page, hide others, update nav highlighting |
| `showSubpage(page)` | 179 | Show sub-page within settings |
| `pageInit(page)` | 181 | Dispatch to page-specific init function |

### Helpers

| Function | Line | Description |
|----------|------|-------------|
| `setEl(id, val)` | 197 | Set `innerHTML` by element ID |
| `setElStyle(id, prop, val)` | 204 | Set CSS style property |
| `setVal(id, val)` | 208 | Set `value` property |
| `setChk(id, val)` | 212 | Set `checked` property |
| `getVal(id)` | 216 | Get `value` by element ID |
| `fmtBytes(b)` | 221 | Format bytes as KB/MB |
| `hexToRgba(hex, a)` | 229 | Hex color → `rgba()` string |
| `togglePass(id)` | 237 | Toggle password visibility |
| `showMsg(id, html, autoClear)` | 242 | Show status message in container |
| `settingsSave(ev, url, form, restart)` | 260 | Generic XHR form POST for settings |
| `applySettingsFlash()` | 288 | Flash save indicator animation |

### Restart Popup

| Function | Line | Description |
|----------|------|-------------|
| `showRestartPopup()` | 300 | Show restart confirmation modal |
| `closeRestart()` | 311 | Close restart modal |
| `confirmRestart()` | 314 | POST `/restart` with countdown |

### Page: Dashboard

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `dbLoadChartJs(cb)` | 338 | — | Lazy-load Chart.js library |
| `dbInit()` | 367 | `/api/filelist` | Initialize dashboard: load file list, build file selector |
| `dbLoadData()` | 395 | `/download?file=...` | Fetch datalog file contents |
| `dbApplyFilters()` | 409 | — | Re-apply UI filters on existing data |
| `dbProcessData(data)` | 415 | — | Parse pipe-delimited log data → chart data |
| `dbRenderChart(data)` | 472 | — | Render bar chart with Chart.js |
| `dbExportCSV()` | 512 | — | Export filtered data as CSV download |

### Page: Files

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `filesInit()` | 534 | `/api/filelist` | Fetch and render file listing |
| `filesRender()` | 547 | — | Render file table HTML from cached data |
| `filesSetStorage(s)` | 607 | — | Switch storage view (internal/sdcard) |
| `filesEnterDir(d)` | 608 | — | Navigate into directory |
| `filesGoUp()` | 609 | — | Navigate to parent directory |
| `filesToggleEdit()` | 614 | — | Toggle delete/rename mode |
| `filesDelete(path)` | 616 | `/delete` | Delete file with confirmation |
| `filesUpload()` | 623 | `/upload` | Multi-file upload with progress |
| `filesMkdir()` | 652 | `/mkdir` | Create directory |
| `showMovePopup(path,name)` | 660 | — | Show move/rename dialog |
| `filesApplyMove()` | 665 | `/move_file` | Execute move/rename |

### Page: Live

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `liveInit()` | 674 | — | Start polling intervals |
| `liveUpdate()` | 699 | `/api/live` | Update live sensor readings (500ms poll) |
| `liveBtn(...)` | 752 | — | Render button state indicator |
| `liveLogsUpdate()` | 758 | `/api/recent_logs` | Update recent log entries (3s poll) |

### Settings: Device

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `sdInit()` | 788 | — | Populate device settings form from cached config |
| `regenDevId()` | 820 | `/api/regen-id` | Regenerate device ID |
| `toggleManualId(id)` | 832 | — | Toggle manual device ID editing |

### Settings: Changelog

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `changelogToggle()` | 844 | — | Toggle changelog card open/close |
| `changelogClose(ev)` | 862 | — | Close changelog from error button |
| `changelogLoad()` | 871 | `/api/changelog` | Fetch + render markdown-style changelog |

### Settings: Flow Meter

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `sfInit()` | 924 | — | Populate flow meter form |

### Settings: Hardware

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `hwInit()` | 940 | — | Populate hardware/pin form |

### Settings: Theme

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `thInit()` | 964 | — | Populate theme color pickers |

### Settings: Network

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `netInit()` | 988 | — | Populate network form |
| `netToggleMode()` | 1012 | — | Toggle AP/Client fields visibility |
| `netToggleStatic()` | 1021 | — | Toggle static IP fields visibility |
| `netScanWifi()` | 1030 | `/wifi_scan_start` | Start WiFi scan |
| `netCheckScan()` | 1037 | `/wifi_scan_result` | Poll for scan results |

### Settings: Time

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `timeInit()` | 1060 | — | Populate time settings form |
| `timeSetManual(ev)` | 1091 | `/set_time` | Manual RTC set |
| `timeSyncNTP(ev)` | 1099 | `/sync_time` | NTP sync |
| `timeRtcProtect(ev)` | 1106 | `/rtc_protect` | Toggle RTC write protect |
| `timeFlushLogs()` | 1112 | `/flush_logs` | Force flush log buffer |
| `timeBackupBoot()` | 1115 | `/backup_bootcount` | Backup boot counter |
| `timeRestoreBoot()` | 1118 | `/restore_bootcount` | Restore boot counter |

### Settings: Datalog

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `dlInit()` | 1125 | — | Populate datalog form + preview |
| `dlLoadFiles()` | 1166 | `/api/filelist` | Load datalog file list for selector |
| `dlDeleteFile(path)` | 1190 | `/delete` | Delete datalog file |
| `dlUpdatePreview()` | 1196 | — | Live preview of datalog format |

### Settings: Import/Export

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `settingsImport()` | 1225 | `/import_settings` | Upload settings JSON with progress |

### OTA Update

| Function | Line | API Called | Description |
|----------|------|-----------|-------------|
| `otaUpload()` | 1241 | `/do_update` | Upload `.bin` firmware with validation (magic byte 0xE9) |

---

## 6. Orchestrator (`Logger.ino`, 342 lines)

### Includes
```
src/core/Globals.h
src/managers/ConfigManager.h
src/managers/HardwareManager.h
src/managers/StorageManager.h
src/managers/RtcManager.h
src/managers/WiFiManager.h
src/managers/DataLogger.h
src/web/WebServer.h
src/utils/Utils.h
```

### `setup()` (lines 35–174)
Boot sequence: early GPIO capture → `initStorage()` → `loadConfig()` → determine wakeup reason → `initHardware()` → WiFi connect/AP → `setupWebServer()` → state machine init.

### `loop()` (lines 176–341)
Main state machine:
- Button debounce polling
- `STATE_IDLE` → `STATE_WAIT_FLOW` → `STATE_MONITORING` → `STATE_DONE`
- Log entry creation + flush
- Deep sleep entry via `configureWakeup()` + `esp_deep_sleep_start()`
- Deferred restart handling (`shouldRestart`)

---

## API → JS → Firmware Call Graph

```
web.js                      API Endpoint              WebServer.cpp → Manager
─────────────────────────────────────────────────────────────────────────────
DOMContentLoaded        →   GET /api/status        →  getRtcDateTimeString(), getStorageInfo()
                        →   GET /export_settings   →  (serializes full config)
dbLoadData()            →   GET /download?file=    →  (serves file from FS)
dbInit()                →   GET /api/filelist      →  scanDir()
filesInit()             →   GET /api/filelist      →  scanDir()
liveUpdate()            →   GET /api/live          →  getRtcDateTimeString(), getStorageInfo()
liveLogsUpdate()        →   GET /api/recent_logs   →  getActiveDatalogFile()
changelogLoad()         →   GET /api/changelog     →  (serves changelog.txt)
regenDevId()            →   POST /api/regen-id     →  WiFi.macAddress()
settingsSave()          →   POST /save_device      →  saveConfig()
                        →   POST /save_flowmeter   →  saveConfig(), backupBootCount()
                        →   POST /save_hardware    →  saveConfig() → restart
                        →   POST /save_theme       →  saveConfig()
                        →   POST /save_datalog     →  saveConfig()
                        →   POST /save_network     →  saveConfig() → restart
                        →   POST /save_time        →  saveConfig()
timeSetManual()         →   POST /set_time         →  Rtc->SetDateTime()
timeSyncNTP()           →   POST /sync_time        →  syncTimeFromNTP()
timeRtcProtect()        →   POST /rtc_protect      →  Rtc->SetIsWriteProtected()
timeFlushLogs()         →   POST /flush_logs       →  flushLogBufferToFS()
timeBackupBoot()        →   POST /backup_bootcount →  backupBootCount()
timeRestoreBoot()       →   POST /restore_bootcount→  restoreBootCount()
filesUpload()           →   POST /upload           →  (multipart file write)
filesDelete()           →   GET /delete            →  deleteRecursive()
filesMkdir()            →   GET /mkdir             →  targetFS->mkdir()
filesApplyMove()        →   GET /move_file         →  targetFS->rename()
netScanWifi()           →   GET /wifi_scan_start   →  WiFi.scanNetworks(true)
netCheckScan()          →   GET /wifi_scan_result  →  WiFi.scanComplete()
confirmRestart()        →   POST /restart          →  shouldRestart = true
otaUpload()             →   POST /do_update        →  Update.begin/write/end()
settingsImport()        →   POST /import_settings  →  deserializeJson → saveConfig()
(failsafe)              →   POST /factory_reset    →  LittleFS.format() → restart
```
