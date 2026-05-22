# Operating the ESP32 Logger

This document covers first boot, operating mode selection, sensor and exporter
configuration, OTA updates, safe mode recovery, diagnostics, and common
troubleshooting scenarios. It assumes firmware has been flashed and the device
is running. For build and flash instructions see [README.md](../README.md).

---

## First boot

On first boot the device has no saved board profile (`/board_profile.txt`
absent), so `g_setupRequired = true` and all HTTP requests redirect to
`/firstrun` (`src/web/WebServer.cpp:328-356`, `ESP_Logger.ino:572`).

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

The wizard collects seven pin fields (`src/web/FirstRunHandler.cpp:146-154`):

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
(`src/web/FirstRunHandler.cpp:157-175`). Strap, USB, flash, and reserved pins
are rejected. Continuous-mode builds can leave `flowSensor`/RTC pins as
`PIN_UNSET` (0xFF) if those features are not wired.

After the wizard submits `POST /api/firstrun`, the board profile is written to
`/board_profile.txt` via `BoardProfiles::save()` and the device reboots.

---

## Operating modes

### Legacy

**When to pick it:** water usage logging where battery life matters.

**Power profile:** deep sleep between button presses; wake on GPIO interrupt
from FF or PF button. Sleep entered via `_doSleep()` (`ESP_Logger.ino:176`),
which calls `esp_deep_sleep_start()`. Sleep macros: `ISR_DEBOUNCE_MICROS`,
`BUTTON_WAIT_FLOW_MS`, `FLOW_IDLE_TIMEOUT_MS` in `src/setup.h`.

**What runs:** the legacy state machine (`STATE_IDLE → STATE_WAIT_FLOW →
STATE_MONITORING → STATE_DONE`), RTC RAM log buffer (`LOG_BATCH_SIZE=16`),
pipe-delimited TXT output to LittleFS or SD.

### Continuous

**When to pick it:** always-powered sensing where real-time data and export
pipelines are needed.

**Power profile:** no deep sleep; `loop()` drives OTA confirm watchdog and
FreeRTOS tasks run permanently (`ESP_Logger.ino:919`). CPU at full frequency.

**What runs:** FreeRTOS pipeline — SensorTask (priority 3), SlowSensorTask
(priority 2), ProcessingTask (priority 2), StorageTask (priority 1), ExportTask
(priority 1). Priorities defined in `src/setup.h:TASK_PRIO_*`.

### Hybrid

**When to pick it:** water logger deployment that also needs environmental
sensor readings.

**Power profile:** no deep sleep (the sensor pipeline requires continuous
operation); deep sleep is blocked when hybrid mode is active
(`ESP_Logger.ino:650`).

**What runs:** both the legacy state machine and the FreeRTOS pipeline.
Flow events are recorded by `FlowRunLogger`; other sensors feed the pipeline.

---

## Sensors

### Adding a sensor

Navigate to **Settings → Core Logic** (or directly to `/settings/sensors`).
Click **+ Add Sensor**, choose a sensor type, and assign pins. Before
rendering the pin picker, `www/js/sensors.js:clLoadBoardProfile()` (line 548)
fetches `/api/board-profiles` and builds a pin dropdown that contains only
GPIOs allowed by the active board profile. Pins not allowed by the profile
are excluded from the selector.

Save using **Save & Restart**. The device writes `platform_config.json` and
reboots to apply.

### I2C address conflicts

Two sensors that share a fixed I2C address cannot coexist. When
`SensorManager` initialises a plugin it calls `_claimI2cAddress(addr, who)`
(`src/sensors/SensorManager.cpp:29`). If the address is already claimed by
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
(`src/export/MqttExporter.cpp:79`). No bundled CA store; connection is
`setInsecure()` until a CA store is shipped (see REFACTORING_GUIDELINES.md §Deferred).

### HTTP

Minimum required fields (`src/export/HttpExporter.cpp:HttpExporter::init`):
`enabled=true`, `url`.

Optional: `method` (default `POST`), `headers` (up to 4 key-value pairs).

TLS: HTTPS URLs automatically use `WiFiClientSecure` with `setInsecure()`
(`src/export/HttpExporter.cpp:79-83`). Header names with non-alphanumeric
characters and header values containing CRLF are rejected at load time.

### Webhook

Minimum required fields (`src/export/WebhookExporter.cpp:8`):
`enabled=true`, `url`.

TLS: `https://` URLs use `WiFiClientSecure` (`src/export/WebhookExporter.cpp:33`).
Webhook failures do not block other exporters.

### SensorCommunity

Minimum required fields: `enabled=true`. The exporter derives the sensor chip
ID from the device MAC address automatically. Requires SDS011 or compatible PM
sensor readings in the pipeline.

TLS: always uses `WiFiClientSecure` (`src/export/SensorCommunityExporter.cpp:25`).

### OpenSenseMap

Minimum required fields (`src/export/OpenSenseMapExporter.cpp:9-10`):
`enabled=true`, `box_id`, `access_token`.

TLS: uses `WiFiClientSecure` (`src/export/OpenSenseMapExporter.cpp:74`).

---

## OTA update

### Upload flow

POST a `.bin` firmware file to `/do_update` (`src/web/WebServer.cpp:1992`).
The handler streams the upload to the OTA partition via the Arduino OTA Update
library. A `sha256=<64-hex>` query parameter triggers SHA-256 verification:
the server hashes each chunk with `mbedtls/sha256.h` and compares the final
digest at upload completion (`src/web/WebServer.cpp:1979`).

The upload endpoint returns 503 if the OTA module is disabled:
`src/web/WebServer.cpp:2069`.

### Auto-confirm and rollback

After a successful flash, the device reboots into the new image with the
partition state `ESP_OTA_IMG_PENDING_VERIFY`. `OtaManager::boot()` arms a
90-second deadline (`OTA_CONFIRM_TIMEOUT_MS` from `src/setup.h`).
`OtaManager::tick(millis())` is called every `loop()` iteration
(`ESP_Logger.ino:843`). If the device stays up for 90 s the image is
confirmed. A crash or hardware watchdog reset before then triggers a
bootloader rollback to the previous partition
(`src/managers/OtaManager.cpp:68-70`).

### OTA kill switch (R20)

`POST /api/modules/ota/enable?on=0&csrf=<token>` disables the OTA upload
path. Subsequent `/do_update` requests return `{"ok":false,"error":"OTA
disabled in /api/modules/ota"}` with HTTP 503 (`src/web/WebServer.cpp:2069`).
Re-enable with `?on=1`.

---

## Safe mode and failsafe page

### Triggers

Safe mode (`g_safeMode = true`) is set in three situations
(`ESP_Logger.ino:508-551`):

1. **Circuit breaker**: `g_consecutiveResets >= 3` non-graceful resets
   (WDT, panic, brownout) without a 60-second clean uptime between them.
   Counts are stored in RTC slow memory (`g_consecutiveResets`).

2. **LittleFS mount failure**: `LittleFS.begin(formatOnFail=false)` returns
   false. The device boots AP-only with no file system access
   (`ESP_Logger.ino:544-551`).

3. **TaskManager init failure**: if `TaskManager::init()` returns false
   (`ESP_Logger.ino:458-461`), safe mode is triggered before any FreeRTOS
   task starts.

### What is reachable in safe mode

The embedded `FAILSAFE_HTML` page (PROGMEM, `src/web/WebServer.cpp:170`) is
served for all routes. It provides: OTA upload, LittleFS file management, and
the Format Filesystem and Factory Reset buttons.

### Format Filesystem button

Use **only** when the device is stuck in safe mode due to LittleFS corruption.
Sends `POST /api/format_filesystem` (`src/web/WebServer.cpp:1529`), which
wipes the entire LittleFS partition and reboots. This erases all config, board
profile, logs, and UI files; the first-run wizard runs after restart.

---

## Diagnostics

### /api/diag fields (R19)

`GET /api/diag` returns a JSON object (`src/web/ApiHandlers.cpp:325`,
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
| `resetLog` | Tail of `/reset_log.txt` (last ≤16 lines) |
| `uptime` | Seconds since boot |
| `network.ip` | Current IP address (client or AP) |
| `ota.running` | Running partition label |
| `ota.pending_verify` | True if image not yet confirmed |
| `ota.rollback_capable` | True if bootloader supports rollback |

### Failsafe diagnostic banner (R19)

When the FAILSAFE_HTML page loads it polls `/api/diag` to display current IP,
uptime, free heap, and consecutive reset count at the top of the page.

### /reset_log.txt format

One line per reset: `<timestamp> boot=<N> reason=<string> gpio=<hex>`.
Written in `_writeResetLog()` (`ESP_Logger.ino:316`).

---

## Troubleshooting

### Device boots into safe mode

**Remedy:** fetch `GET /api/diag` and check `counters.resets` and the
`resetLog` array. If resets ≥ 3, the circuit breaker fired — identify the
crash from `resetLog`. A clean power-cycle that stays up for 60 s clears the
counter (`ESP_Logger.ino:850-853`). If `resetLog` shows a LittleFS mount
error, use the Format Filesystem button in the failsafe UI.

Source: `ESP_Logger.ino:508-551`.

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

Source: `src/sensors/SensorManager.cpp:29` (`_claimI2cAddress`).

### OTA upload returns 503

**Remedy:** re-enable OTA via
`POST /api/modules/ota/enable?on=1&csrf=<token>`.
Obtain the CSRF token from `GET /api/csrf-token`.

Source: `src/web/WebServer.cpp:2069`.

### WiFi save returns restartRequired: true

**Remedy:** WiFi configuration cannot be applied live because
`WiFiModule::start()` returns `false` (`src/modules/WiFiModule.h:26`).
POST to `/restart` to reboot with the new settings.

### First-run wizard keeps appearing after reboot

**Remedy:** `BoardProfiles::load()` reads `/board_profile.txt`
(`src/core/BoardProfiles.h:BoardProfiles::load`). If the file is missing or
names an unknown profile, `g_setupRequired` stays true. Download the file from
the Files page to verify it exists and contains `profile=<known-id>`. If
absent, complete the wizard again.
