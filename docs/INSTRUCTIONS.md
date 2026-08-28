# Operating the ESP32 Logger

This document covers first boot, operating mode selection, sensor and exporter
configuration, OTA updates, safe mode recovery, diagnostics, and common
troubleshooting scenarios. It assumes firmware has been flashed and the device
is running. For build and flash instructions see [README.md](../README.md).

---

## First boot

On first boot the device has no saved board profile (`/board_profile.txt`
absent), so `g_setupRequired = true` and all HTTP requests redirect to
`/firstrun` (`FirstRunGateHandler` in `src/web/WebServer.cpp`; `g_setupRequired` is set in `ESP_Logger.ino`).

### Stage 1 — Board profile

Select the board that matches your hardware from the profile list. The profiles
are:

| Short id | Board |
|---|---|
| `xiao_c3` | Seeed XIAO ESP32-C3 |
| `supermini_c3` | ESP32-C3 SuperMini |
| `generic_c3` | Generic ESP32-C3 |
| `generic_s3` | Generic ESP32-S3 |
| `custom` | Custom — all pins allowed |

Source: `src/core/BoardProfiles.cpp` (profile structs).

### Stage 2 — Operating mode

| Mode | When to use |
|---|---|
| `legacy` | Water logger only; deep sleep between events |
| `continuous` | Multi-sensor pipeline; no deep sleep |
| `hybrid` | Water logger AND sensor pipeline together |

### Stage 3 — Pin assignment (legacy and hybrid only)

The wizard collects seven pin fields (the `PinAssignment` table in `handlePostFirstRun()`, `src/web/FirstRunHandler.cpp`):

| Field | Purpose |
|---|---|
| `wifiTrigger` | GPIO that wakes the device into AP/web mode |
| `wakeupFF` | Full-flush (FF) button |
| `wakeupPF` | Part-flush (PF) button |
| `flowSensor` | YF-S201/S403 pulse input |
| `rtcCE` | DS1302 RTC chip-enable |
| `rtcIO` | DS1302 serial data |
| `rtcSCLK` | DS1302 serial clock |

Pins are validated against the active board profile before being accepted
(`handlePostFirstRun()` in `src/web/FirstRunHandler.cpp`). Strap, USB, flash, and reserved pins
are rejected. Continuous-mode builds can leave `flowSensor`/RTC pins as
`PIN_UNSET` (0xFF) if those features are not wired.

After the wizard submits `POST /api/firstrun`, the board profile is written to
`/board_profile.txt` via `BoardProfiles::save()` and the device reboots.

---

## Operating modes

### Legacy

**When to pick it:** water usage logging where battery life matters.

**Power profile:** deep sleep between button presses; wake on GPIO interrupt
from FF or PF button. Sleep entered via `_doSleep()` (`ESP_Logger.ino`),
which calls `esp_deep_sleep_start()`. Sleep macros: `ISR_DEBOUNCE_MICROS`,
`BUTTON_WAIT_FLOW_MS`, `FLOW_IDLE_TIMEOUT_MS` in `src/setup.h`.

**What runs:** the legacy state machine (`STATE_IDLE → STATE_WAIT_FLOW →
STATE_MONITORING → STATE_DONE`), RTC RAM log buffer (`LOG_BATCH_SIZE=16`),
pipe-delimited TXT output to LittleFS or SD.

### Continuous

**When to pick it:** always-powered sensing where real-time data and export
pipelines are needed.

**Power profile:** no deep sleep; `loop()` drives OTA confirm watchdog and
FreeRTOS tasks run permanently (`ESP_Logger.ino`). CPU at full frequency.

**What runs:** FreeRTOS pipeline — SensorTask (priority 3), SlowSensorTask
(priority 2), ProcessingTask (priority 2), StorageTask (priority 1), ExportTask
(priority 1). Priorities defined in `src/setup.h:TASK_PRIO_*`.

### Hybrid

**When to pick it:** water logger deployment that also needs environmental
sensor readings.

**Power profile:** no deep sleep (the sensor pipeline requires continuous
operation); deep sleep is blocked when hybrid mode is active
(the hybrid timer-wake path in `setup()`, `ESP_Logger.ino`).

**What runs:** both the legacy state machine and the FreeRTOS pipeline.
Flow events are recorded by `FlowRunLogger`; other sensors feed the pipeline.

---

## Sensors

### Adding a sensor

Navigate to **Settings → Core Logic** (or directly to `/settings/sensors`).
Click **+ Add Sensor**, choose a sensor type, and assign pins. Before
rendering the pin picker, `clLoadBoardProfile()` in `www/js/sensors.js`
fetches `/api/board-profiles` and builds a pin dropdown that contains only
GPIOs allowed by the active board profile. Pins not allowed by the profile
are excluded from the selector.

Save using **Save & Restart**. The device writes `platform_config.json` and
reboots to apply.

### I2C address conflicts

Two sensors that share a fixed I2C address cannot coexist. When
`SensorManager` initialises a plugin it calls `_claimI2cAddress(addr, who)`
(`src/sensors/SensorManager.cpp`). If the address is already claimed by
another plugin, the second sensor fails to init and logs:

```
[<sensorId>] I2C address 0x<XX> already claimed by <other>
```

To resolve: remove one of the conflicting sensors from `platform_config.json`
via the UI, or change the `address` field if the sensor supports an alternate
address (e.g. BME280 supports `0x76` or `0x77`).

### Sensor types

| Type id | Name | Interface | Metrics |
|---|---|---|---|
| `bme280` | BME280/BMP280 | I2C (`sda`, `scl`, `address`) | temperature, humidity, pressure |
| `bme688` | BME688/680 | I2C (`sda`, `scl`, `address`) | temperature, humidity, pressure, gas_resistance |
| `ds18b20` | DS18B20 | 1-Wire (`pin`) | temperature (×N probes on bus) |
| `sds011` | SDS011 | UART (`uart_rx`, `uart_tx`) | pm25, pm10 |
| `pms5003` | PMS5003 | UART (`uart_rx`, `uart_tx`) | pm1, pm25, pm10 |
| `ens160` | ENS160 | I2C (`sda`, `scl`, `address`) | tvoc, eco2, aqi |
| `sgp30` | SGP30 | I2C (`sda`, `scl`) | tvoc, eco2 |
| `scd4x` | SCD40/41 | I2C (`sda`, `scl`) | co2, temperature, humidity |
| `veml6075` | VEML6075 | I2C (`sda`, `scl`) | uva, uvb, uv_index |
| `veml7700` | VEML7700 | I2C (`sda`, `scl`) | lux, white |
| `bh1750` | BH1750 | I2C (`sda`, `scl`, `address`) | lux |
| `yfs201` | YF-S201 water flow | GPIO/ISR (`pin`) | flow_rate, volume |
| `yfs403` | YF-S403 water flow | GPIO/ISR (`pin`) | flow_rate, volume |
| `water_flow` | Custom flow sensor | GPIO/ISR (`pin`, `pulses_per_liter`) | flow_rate, volume |
| `rain` | Tipping-bucket rain | GPIO/ISR (`pin`) | rain_rate, rain_total |
| `wind` | Anemometer + vane | GPIO/ADC (`pin`, `dir_pin`) | wind_speed, wind_direction |
| `soil_moisture` | Capacitive soil | ADC (`pin`) | moisture_pct, moisture_raw |
| `hcsr04` | HC-SR04 distance | GPIO (`trig_pin`, `echo_pin`) | distance |
| `zmpt101b` | ZMPT101B AC voltage | ADC (`pin`) | voltage_vrms, voltage_raw |
| `zmct103c` | ZMCT103C AC current | ADC (`pin`) | current_arms, current_raw |

Sources: `src/sensors/plugins/*.h` class-header comment blocks.

---

## Exporters

### MQTT

Minimum required fields (`src/export/MqttExporter.cpp:MqttExporter::init`):
`enabled=true`, `broker` (host or IP).

Optional: `port` (default 1883), `topic_prefix` (default `waterlogger`),
`username`, `password`, `qos` (only 0 is supported), `retain`, `ha_discovery`,
`use_tls`.

TLS: when `use_tls=true`, a `WiFiClientSecure` is used
(`MqttExporter::send()`). No bundled CA store; connection is
`setInsecure()` until a CA store is shipped (see REFACTORING_GUIDELINES.md §Deferred).

### HTTP

Minimum required fields (`src/export/HttpExporter.cpp:HttpExporter::init`):
`enabled=true`, `url`.

Optional: `method` (default `POST`), `headers` (up to 4 key-value pairs).

TLS: HTTPS URLs automatically use `WiFiClientSecure` with `setInsecure()`
(`HttpExporter::send()`). Header names with non-alphanumeric
characters and header values containing CRLF are rejected at load time.

### Webhook

Minimum required fields (`WebhookExporter::init()`):
`enabled=true`, `url`.

TLS: `https://` URLs use `WiFiClientSecure` (`WebhookExporter::_fireRule()`).
Webhook failures do not block other exporters.

### SensorCommunity

Minimum required fields: `enabled=true`. The exporter derives the sensor chip
ID from the device MAC address automatically. Requires SDS011 or compatible PM
sensor readings in the pipeline.

TLS: always uses `WiFiClientSecure` (`SensorCommunityExporter::_postPin()`).

### OpenSenseMap

Minimum required fields (`OpenSenseMapExporter::init()`):
`enabled=true`, `box_id`, `access_token`.

TLS: uses `WiFiClientSecure` (`OpenSenseMapExporter::send()`).

---

## OTA update

### Upload flow

POST a `.bin` firmware file to `/do_update` (registered in `setupWebServer()`, `src/web/WebServer.cpp`).
The handler streams the upload to the OTA partition via the Arduino OTA Update
library. A `sha256=<64-hex>` query parameter triggers SHA-256 verification:
the server hashes each chunk with `mbedtls/sha256.h` and compares the final
digest at upload completion (`mbedtls_sha256_finish` in the `/do_update` upload handler).

The upload endpoint returns 503 if the OTA module is disabled:
the `OTA disabled in /api/modules/ota` guard in `src/web/WebServer.cpp`.

### Auto-confirm and rollback

After a successful flash, the device reboots into the new image with the
partition state `ESP_OTA_IMG_PENDING_VERIFY`. `OtaManager::boot()` arms a
90-second deadline (`OTA_CONFIRM_TIMEOUT_MS` from `src/setup.h`).
`OtaManager::tick(millis())` is called every `loop()` iteration
(`ESP_Logger.ino`). If the device stays up for 90 s the image is
confirmed. A crash or hardware watchdog reset before then triggers a
bootloader rollback to the previous partition
(`OtaManager::boot()`).

### OTA kill switch (R20)

`POST /api/modules/ota/enable?on=0&csrf=<token>` disables the OTA upload
path. Subsequent `/do_update` requests return `{"ok":false,"error":"OTA
disabled in /api/modules/ota"}` with HTTP 503 (the same guard in `src/web/WebServer.cpp`).
Re-enable with `?on=1`.

---

## Safe mode and failsafe page

### Triggers

Safe mode (`g_safeMode = true`) is set in three situations
(`setup()` in `ESP_Logger.ino`):

1. **Circuit breaker**: `g_consecutiveResets >= 3` non-graceful resets
   (WDT, panic, brownout) without a 60-second clean uptime between them.
   Counts are stored in RTC slow memory (`g_consecutiveResets`).

2. **LittleFS mount failure**: `LittleFS.begin(formatOnFail=false)` returns
   false. The device boots AP-only with no file system access
   (`setup()` in `ESP_Logger.ino`).

3. **TaskManager init failure**: if `TaskManager::init()` returns false
   (`ESP_Logger.ino`), safe mode is triggered before any FreeRTOS
   task starts.

### What is reachable in safe mode

The embedded failsafe page is served for all routes by `sendFailsafePage()`
(`src/web/WebServer.cpp`). It provides: OTA upload, LittleFS file
management, and the Format Filesystem and Factory Reset buttons.

It lives in PROGMEM, stored **gzipped** as `FAILSAFE_HTML_GZ`
(`src/web/FailsafeHtml.h`, generated from `src/web/failsafe.html` by
`scripts/gen_failsafe.py`) — 8 KB of flash instead of 27 KB. It is still
linked into the firmware image, never read from the filesystem it exists to
repair. A client that does not send `Accept-Encoding: gzip` — curl, unless
given `--compressed` — is served `FAILSAFE_PLAIN` instead
(`src/web/WebServer.cpp`): a ~1 KB JavaScript-free page carrying the
upload and restart forms, so a shell rescue is not a binary dump.

### Format Filesystem button

Use **only** when the device is stuck in safe mode due to LittleFS corruption.
Sends `POST /api/format_filesystem` (`h_post_api_format_filesystem()` in `src/web/WebServer.cpp`), which
wipes the entire LittleFS partition and reboots. This erases all config, board
profile, logs, and UI files; the first-run wizard runs after restart.

---

## Diagnostics

### /api/diag fields (R19)

`GET /api/diag` returns a JSON object (`src/web/ApiHandlers.cpp`,
`handleApiDiag`):

| Field | Contents |
|---|---|
| `heap.free` | Current free heap bytes |
| `heap.min` | Minimum free heap seen since boot |
| `heap.largestBlock` | Largest contiguous free block |
| `heap.fragPct` | Heap fragmentation percentage |
| `tasks.*` | FreeRTOS stack high-water marks per task (words remaining) |
| `counters.queueDrops` | Cumulative queue-full drops |
| `counters.ringPushDrops` | Ring buffer push drops |
| `counters.resets` | Current consecutive non-graceful reset count |
| `resetLog` | Tail of `/error_log.txt` (last ≤16 lines). The JSON key keeps its old name so saved diagnostic bundles and the failsafe page still parse. |
| `uptime` | Seconds since boot |
| `network.ip` | Current IP address (client or AP) |
| `storage.sd_supported` | False when the firmware was built with `FEATURE_SD_STORAGE` off — distinguishes "no driver" from "no card" |
| `storage.sd_available` | True when a card is present and mounted right now |
| `storage.littlefs` | True when LittleFS mounted |
| `storage.view` | Active storage view: `sdcard` or `internal` |
| `ota.running` | Running partition label |
| `ota.pending_verify` | True if image not yet confirmed |
| `ota.rollback_capable` | True if bootloader supports rollback |

### Failsafe diagnostic banner (R19)

When the failsafe page loads it polls `/api/diag` to display current IP,
uptime, free heap, and consecutive reset count at the top of the page.

### /error_log.txt format

The device's one diagnostic log. It was called `/reset_log.txt` in builds before
the ESP-NOW clock-drift line was added — the old name described only its first
writer — and `eventLogMigrate()` renames it at boot, so existing history
survives the change.

Every writer goes through `eventLogPrintf()` in `src/core/EventLog.h`, which
appends one line, takes `fsMutex`, and resolves the filesystem the same way
`/api/diag` reads it back. Lines are capped at 160 characters.

Current writers:

| Line | Written by |
|------|-----------|
| `boot#N  reason=<string>` | `_writeResetLog()` (`ESP_Logger.ino`), once per notable reset |
| `boot#N  OTA_<event>  running=<label>` | `_logOtaEvent()` (`OtaManager.cpp`) |
| `boot#N  ESPNOW_SKEW  node=<id>  skew=<±N> s` | `acceptFrame()` (`EspNowIngest.cpp`), when a node's clock is more than `ESPNOW_SKEW_WARN_S` out, at most once an hour per node |

---

## Troubleshooting

### Device boots into safe mode

**Remedy:** fetch `GET /api/diag` and check `counters.resets` and the
`resetLog` array. If resets ≥ 3, the circuit breaker fired — identify the
crash from `resetLog`. A clean power-cycle that stays up for 60 s clears the
counter (the reset-counter clear in `setup()`, `ESP_Logger.ino`). If `resetLog` shows a LittleFS mount
error, use the Format Filesystem button in the failsafe UI.

Source: `setup()` in `ESP_Logger.ino`.

### Sensor refuses to init: "pin not assigned"

**Remedy:** open **Settings → Core Logic**, find the sensor, and assign a
GPIO. Alternatively re-run the wizard by removing `/board_profile.txt` from
the Files page (the wizard starts whenever that file is absent).

Source: `src/core/BoardProfiles.h:validateAttachPin`.

### I2C address conflict

**Remedy:** open `platform_config.json` via the Files page and change the
`address` field of the conflicting sensor to an alternate address if the
sensor supports one. VEML6075 and VEML7700 share fixed address `0x10` and
cannot coexist on the same bus.

Source: `_claimI2cAddress()` in `src/sensors/SensorManager.cpp`.

### OTA upload returns 503

**Remedy:** re-enable OTA via
`POST /api/modules/ota/enable?on=1&csrf=<token>`.
Obtain the CSRF token from `GET /api/csrf-token`.

Source: the `OTA disabled in /api/modules/ota` guard in `src/web/WebServer.cpp`.

### WiFi save returns restartRequired: true

**Remedy:** WiFi configuration cannot be applied live because
`WiFiModule::start()` returns `false` (`src/modules/WiFiModule.h`).
POST to `/restart` to reboot with the new settings.

### First-run wizard keeps appearing after reboot

**Remedy:** `BoardProfiles::load()` reads `/board_profile.txt`
(`src/core/BoardProfiles.h:BoardProfiles::load`). If the file is missing or
names an unknown profile, `g_setupRequired` stays true. Download the file from
the Files page to verify it exists and contains `profile=<known-id>`. If
absent, complete the wizard again.
