# Project Comparison — Modular vs Monolith

> **Water Logger** · ESP32-C3 (XIAO)  
> Generated: 2026-02-25

---

## At a Glance

| Metric | Monolith (`full_logger.ino`) | Modular (multi-file) |
|--------|------------------------------|----------------------|
| **Version** | v4.1.4 | v4.1.5 |
| **Source files** | 1 | 17 (C++) + 4 (frontend) |
| **Total C++ lines** | 5,341 | ~2,900 |
| **Total source size** | 286 KB | ~160 KB C++ + ~169 KB frontend |
| **Flash used by UI** | Embedded in firmware | Separate files on LittleFS `/www/` |
| **UI architecture** | Server-side rendered (multi-page) | Client-side SPA (single page) |

---

## Structural Comparison

### File Organization

```
MONOLITH                              MODULAR
──────────────────────                ──────────────────────
full_logger.ino (5341 lines)          Logger.ino (342 lines)
                                      src/core/Config.h (241 lines)
   ↳ enums, structs, consts             ↳ enums, structs, consts
                                      src/core/Globals.h/.cpp (204 lines)
   ↳ global variables                   ↳ global variables
                                      src/managers/
   ↳ utility functions                   ConfigManager.cpp (413 lines)
   ↳ ISR handlers                        DataLogger.cpp (158 lines)
   ↳ NTP, bootcount                      HardwareManager.cpp (80 lines)
   ↳ data logging                        RtcManager.cpp (198 lines)
   ↳ config management                   StorageManager.cpp (132 lines)
   ↳ storage/hardware init               WiFiManager.cpp (139 lines)
   ↳ WiFi functions                   src/utils/Utils.cpp (51 lines)
   ↳ JSON/HTML helpers                src/web/
   ↳ setupWebServer() (3020 lines!)     WebServer.cpp (1438 lines)
   ↳ setup() + loop()                www/
                                        index.html (96 KB)
                                        web.js (61 KB, 1292 lines)
                                        style.css (11 KB)
                                        chart.min.js (205 KB)
```

---

## Function-by-Function Parity

### ✅ Identical Functions (Present in Both)

| Category | Functions | Monolith Lines | Modular File |
|----------|-----------|---------------|--------------|
| **Config** | `loadDefaultConfig`, `loadConfig`, `saveConfig`, `migrateConfig`, `generateDeviceId`, `regenerateDeviceId` | 851–1123 | `ConfigManager.cpp` |
| **Logging** | `flushLogBufferToFS`, `addLogEntry` | 688–849 | `DataLogger.cpp` |
| **Hardware** | `initHardware`, `debounceButton`, `onFFButton`, `onPFButton`, `onFlowPulse` | 552–599, 1174–1325 | `HardwareManager.cpp` |
| **RTC** | `initRtc`¹, `backupBootCount`, `restoreBootCount`, `getRtcTimeString`, `getRtcDateTimeString`, `configureWakeup`, `getWakeupReason` | 653–686, 1416–1479, 1844–1873 | `RtcManager.cpp` |
| **Storage** | `initStorage`, `getCurrentViewFS`, `getActiveDatalogFile`, `getStorageInfo`, `getStorageBarColor`, `generateDatalogFileOptions`, `countDatalogFiles` | 1125–1172, 1686–1842 | `StorageManager.cpp` |
| **WiFi** | `connectToWiFi`, `startAPMode`, `safeWiFiShutdown`, `syncTimeFromNTP` | 1327–1414, 601–651 | `WiFiManager.cpp` |
| **Utils** | `formatFileSize`, `buildPath`, `sanitizePath`, `sanitizeFilename`, `deleteRecursive` | 450–550, 4896–4918 | `Utils.cpp` |
| **Web** | `getModeDisplay`, `getNetworkDisplay`, `sendJsonResponse`, `sendRestartPage` | 467–491, 1487–1512 | `WebServer.cpp` |
| **Main** | `setup`, `loop` | 4920–5341 | `Logger.ino` |

¹ `initRtc()` is called directly from `initHardware()` in the modular project, embedded inline in monolith's `initHardware()`.

### ❌ Monolith-Only Functions (7)

These handle **server-side HTML rendering** — replaced by `www/index.html` + `www/web.js` in the modular version:

| Function | Lines | Purpose | Modular Replacement |
|----------|-------|---------|---------------------|
| `icon(emoji)` | 474–478 | Conditional emoji rendering | CSS + `applyStatus()` in `web.js` |
| `getThemeClass()` | 480–486 | Returns `"dark-theme"` CSS class | `applyStatus()` sets CSS variables client-side |
| `getCurrentPage(title)` | 1514–1524 | Nav item active highlighting | `navigateTo()` in `web.js` |
| `writeSidebar(out, page)` | 1526–1562 | Desktop sidebar HTML | Static HTML in `index.html` |
| `writeBottomNav(out, page)` | 1564–1589 | Mobile bottom nav HTML | Static HTML in `index.html` |
| `sendChunkedHtml(r, title, bodyWriter)` | 1591–1680 | Full page HTML template (head, theme, body) | Static HTML in `index.html` |
| `writeFileList(out, dir, editMode)` | 1696–1752 | Server-side file listing HTML | `filesRender()` in `web.js` |

### ❌ Modular-Only Components

| Component | Lines | Purpose |
|-----------|-------|---------|
| `applyDefaults()` in `ConfigManager.cpp` | 7–80 | Post-load sanity defaults (internal helper) |
| `sanitizeWakeConfig()` in `ConfigManager.cpp` | 82–111 | Validate wake pin config |
| Failsafe HTML in `WebServer.cpp` | 69–384 | Recovery UI when `/www/index.html` missing |
| `getMime()` in `WebServer.cpp` | 389–402 | MIME type helper for static file serving |
| `scanDir()` JSON helper in `WebServer.cpp` | 407–444 | File listing → JSON array |
| `fmtIP()` in `WebServer.cpp` | 449–451 | IP array to string |
| Entire `www/web.js` | 1292 lines | 94 frontend functions (SPA logic) |
| `www/index.html` | ~2600 lines | Full SPA shell HTML |
| `www/style.css` | ~300 lines | All CSS styling |

---

## API Endpoint Comparison

### ✅ Shared Endpoints (Both Projects)

| Endpoint | Method | Notes |
|----------|--------|-------|
| `/api/status` | GET | Same JSON schema |
| `/api/live` | GET | Same JSON schema |
| `/api/recent_logs` | GET | Same JSON schema |
| `/api/changelog` | GET | Same |
| `/api/regen-id` | POST | Same |
| `/export_settings` | GET | Same JSON schema |
| `/import_settings` | POST | Same |
| `/save_device` | POST | Same params |
| `/save_flowmeter` | POST | Same params |
| `/save_hardware` | POST | Same params → restart |
| `/save_theme` | POST | Same params |
| `/save_datalog` | POST | Same params + `action=create` |
| `/save_network` | POST | Same params → restart |
| `/save_time` | POST | Same params |
| `/set_time` | POST | Same |
| `/sync_time` | POST | Monolith: `/sync_ntp`, Modular: `/sync_time` ⚠️ |
| `/rtc_protect` | POST | Same |
| `/flush_logs` | POST | Same |
| `/backup_bootcount` | POST | Same |
| `/restore_bootcount` | POST | Same |
| `/restart` | GET/POST | Same |
| `/factory_reset` | POST | Same |
| `/download` | GET | Same |
| `/delete` | GET | Same |
| `/mkdir` | GET | Same |
| `/move_file` | GET | Same |
| `/upload` | POST | Same |
| `/wifi_scan_start` | GET | Same |
| `/wifi_scan_result` | GET | Same |
| `/do_update` | POST | Same |

### ⚠️ Endpoint Differences

| Aspect | Monolith | Modular |
|--------|----------|---------|
| NTP sync path | `/sync_ntp` | `/sync_time` |
| File list API | `/api/files?dir=/` | `/api/filelist?dir=/&storage=...` |
| Config export | `/api/config` | `/export_settings` |
| HTML pages | `/`, `/data`, `/settings`, `/files`, `/live` (separate pages) | `/` only (SPA with hash routing `#dashboard`, `#files`, etc.) |
| SPA redirects | N/A | `/dashboard`, `/files`, `/live`, `/settings*` → redirect to `/` |
| Failsafe UI | N/A (always embedded) | `/setup` always serves recovery HTML |

---

## UI / Frontend Comparison

| Aspect | Monolith | Modular |
|--------|----------|---------|
| **Rendering** | Server-side (C++ writes HTML chunks) | Client-side (browser JS renders all UI) |
| **Navigation** | Full page reload per page | Hash-based SPA (`#dashboard`, `#files`, etc.) |
| **Theme** | Server injects CSS class + inline vars | JS sets CSS custom properties on `:root` |
| **Chart.js** | CDN or local `/chart.min.js` | Lazy-loaded from configured path |
| **CSS** | External `/style.css` + inline styles in HTML | External `style.css` from `/www/` |
| **JS** | Inline `<script>` per page (~200–400 lines each) | Single `web.js` file (1292 lines, 94 functions) |
| **File manager** | Server renders file rows in C++ | JS fetches `/api/filelist` → renders client-side |
| **Settings** | Server pre-fills form values in HTML | JS fetches `/export_settings` → populates forms |
| **UI updates** | Requires firmware reflash | Upload new files to `/www/` |
| **Bundle size** | ~0 KB extra (in firmware) | ~373 KB (index.html + web.js + style.css + chart.min.js) |
| **RAM impact** | Higher (chunked HTML generation in heap) | Lower (static file serving, no HTML generation) |

---

## Advantages & Trade-offs

### Monolith Advantages
- ✅ **Zero-dependency UI** — UI always works, no missing files possible
- ✅ **Single flash** — one `.bin` deploys everything
- ✅ **No filesystem corruption risk** — UI can't be accidentally deleted

### Modular Advantages
- ✅ **Separation of concerns** — C++ firmware vs HTML/JS/CSS
- ✅ **Hot-swappable UI** — update UI without reflashing firmware
- ✅ **Faster development** — edit frontend files independently
- ✅ **Lower RAM usage** — serves static files instead of generating HTML
- ✅ **SPA experience** — no full page reloads, smoother UX
- ✅ **Smaller firmware binary** — ~160 KB C++ vs 286 KB monolith
- ✅ **Failsafe recovery** — `/setup` page always available if UI files break
- ✅ **OTA UI updates** — upload new files without firmware change

### Shared
- Same ESP32-C3 hardware target
- Same config struct layout (`DeviceConfig`)
- Same deep sleep / wake-up logic
- Same state machine (IDLE → WAIT_FLOW → MONITORING → DONE)
- Same data logging format
- Same API surface (with minor path differences)
