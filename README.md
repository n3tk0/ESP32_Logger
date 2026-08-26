<div align="center">

# ESP32 Logger

**Multi-sensor environmental logging platform for embedded ESP32 targets.**

Firmware for `xiao_esp32c3`, `esp32c3_supermini`, `lolin_c3_pico`, `esp32s3`, `xiao_esp32s3` and `esp32s3_n16r8` — water flow, air quality, weather, and power monitoring with on-device web UI, MQTT/HTTP/sensor.community/openSenseMap exporters, and OTA updates.

[![Build Firmware](https://github.com/n3tk0/ESP32_Logger/actions/workflows/build-firmware.yml/badge.svg)](https://github.com/n3tk0/ESP32_Logger/actions/workflows/build-firmware.yml)
[![Build Bootloader](https://github.com/n3tk0/ESP32_Logger/actions/workflows/build-bootloader.yml/badge.svg)](https://github.com/n3tk0/ESP32_Logger/actions/workflows/build-bootloader.yml)
[![Platform](https://img.shields.io/badge/platform-ESP32%20%7C%20ESP32--C3%20%7C%20ESP32--S3-blue)](https://platformio.org/)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%7C%20FreeRTOS-00979D)](https://www.arduino.cc/)
[![PlatformIO](https://img.shields.io/badge/built%20with-PlatformIO-orange?logo=platformio)](https://platformio.org/)
[![License](https://img.shields.io/badge/license-See%20LICENSE-lightgrey)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/n3tk0/ESP32_Logger)](https://github.com/n3tk0/ESP32_Logger/commits/main)

</div>

---

## Table of contents

- [Features](#features)
- [Supported boards](#supported-boards)
- [Quick start](#quick-start)
- [Project structure](#project-structure)
- [Documentation](#documentation)
- [Hardware](#hardware)
- [Contributing](#contributing)
- [License](#license)
- [Support the project](#support-the-project)

---

## Features

### Sensors

Toggled via `#ifdef SENSOR_*_ENABLED` in `src/setup.h`. Default build enables only `SENSOR_BME280_ENABLED` and `SENSOR_SDS011_ENABLED`; all others are commented out.

| Macro | Sensor | Interface |
|---|---|---|
| `SENSOR_BME280_ENABLED` | BME280/BMP280 temp/humidity/pressure | I2C |
| `SENSOR_BME688_ENABLED` | BME680/688 temp/humidity/pressure + gas + IAQ + dew point | I2C |
| `SENSOR_DS18B20_ENABLED` | DS18B20 temperature (1-Wire) | GPIO |
| `SENSOR_SDS011_ENABLED` | SDS011 PM2.5/PM10 | UART |
| `SENSOR_PMS5003_ENABLED` | PMS5003 PM1/2.5/10 | UART |
| `SENSOR_SPS30_ENABLED` | Sensirion SPS30 PM1/2.5/4/10 + fan/laser health | I2C |
| `SENSOR_ENS160_ENABLED` | ENS160 TVOC/eCO2/AQI | I2C |
| `SENSOR_SGP30_ENABLED` | SGP30 TVOC/eCO2 | I2C |
| `SENSOR_SCD4X_ENABLED` | SCD40/41 CO2/temp/humidity | I2C |
| `SENSOR_VEML6075_ENABLED` | VEML6075 UV-A/B/index | I2C (0x10) |
| `SENSOR_VEML7700_ENABLED` | VEML7700 ambient light | I2C (0x10) |
| `SENSOR_BH1750_ENABLED` | BH1750 lux | I2C |
| `SENSOR_WATERFLOW_ENABLED` | YF-S201/YF-S403 water flow | GPIO/ISR |
| `SENSOR_RAIN_ENABLED` | Tipping-bucket rain gauge | GPIO/ISR |
| `SENSOR_WIND_ENABLED` | Anemometer + wind vane | GPIO/ADC |
| `SENSOR_SOIL_ENABLED` | Capacitive soil moisture | ADC |
| `SENSOR_HCSR04_ENABLED` | HC-SR04 ultrasonic distance | GPIO |
| `SENSOR_ZMPT101B_ENABLED` | ZMPT101B AC voltage | ADC |
| `SENSOR_ZMCT103C_ENABLED` | ZMCT103C AC current | ADC |

### I2C buses

Each I2C sensor takes a `"bus"` key (`0` by default, or `1`) alongside its
`sda`/`scl` pins. Devices with a fixed, non-selectable address have to go on
different buses to coexist — VEML6075 and VEML7700 are both hard-wired to
`0x10`, so that pair needs one on each.

Bus 1 exists only on parts with two I2C controllers (ESP32-S3, classic ESP32).
The ESP32-C3 has one, and a sensor configured for bus 1 there is refused at
init with an explicit log line rather than failing as "not found".

A bus is brought up once, by the first sensor that claims it. A later sensor
asking for the same bus with *different* pins is refused instead of silently
moving the peripheral out from under the sensors already on it. `GET /api/diag`
reports which controllers exist and which are up, on which pins.

A second bus needs its own pull-up resistors, not just its own pins.

### Memory and history depth

FS-backed history is not wired up yet, so the in-memory ring buffer is the only
store of recent readings. Its capacity is chosen at boot:

| Memory | Budget | Entries | ~Retention at 19 metrics / 10 s |
|---|---|---|---|
| Internal SRAM (C3, or S3 without PSRAM) | 16 KB | ~227 | ~2 min |
| PSRAM (`esp32s3_n16r8`) | 4 MB, capped at 50 % of free PSRAM | ~58 000 | ~8 h |

**What the extra depth reaches today:** `/api/latest`, the per-card sparklines,
and anything else asking for recent values — those all read the newest end of
the ring and benefit immediately.

**What it does not reach yet:** `/api/data` charts. That endpoint materialises
raw readings into a fixed 300-entry array before aggregating, so one request
sees the newest ~300 readings (~2.5 min) no matter how deep the ring is.
Serving hours in a single request needs aggregation that accumulates per bucket
while scanning the ring instead of copying raw readings out first — the same
work the planned CSV reader ("chunk F") implies for the filesystem side.

PSRAM is volatile — a reboot loses it. It extends live retention, it does not
replace CSV logging to flash.

`GET /api/diag` reports `psram` (size/free) and `ring` (capacity, used, bytes,
whether it landed in PSRAM). A `psram.size` of 0 on a board that has PSRAM
fitted almost always means the build targets the wrong SPI mode — see below.

**Octal PSRAM (`…R8` modules) needs `board_build.arduino.memory_type = qio_opi`.**
The stock `esp32-s3-devkitc-1` definition builds for `qio_qspi`, which cannot
talk to an octal part; the RAM is then silently absent with only a line in the
boot log. Boards with *quad* PSRAM (`…R2`, `…R8V`) need `qio_qspi` instead — the
mirror-image failure.

This is why N16R8 is a **separate `esp32s3_n16r8` env** rather than a change to
`esp32s3`: the 16 MB partition table puts LittleFS past the end of an 8 MB
part, so flashing it to a stock DevKitC-1-N8 fails to mount and boots into safe
mode. Build `esp32s3` for 8 MB boards, `esp32s3_n16r8` for 16 MB + PSRAM ones.

The ring is touched from tasks only. It must never be read or written from an
ISR — PSRAM is unreachable whenever the flash cache is disabled, and this
project has `IRAM_ATTR` handlers for flow, rain and wind. It is also allocated
only on the continuous/hybrid path; legacy and safe mode never start the task
that fills it, so they leave it unallocated rather than reserving megabytes for
a buffer nothing writes to.

### Actuators

| Macro | Module | Interface |
|---|---|---|
| `MODULE_HEATER_ENABLED` | Enclosure heater — frost and condensation protection | GPIO/PWM |

Off by default. Drives a MOSFET gate to hold a sensor enclosure above both a
frost setpoint and the measured dew point, which is what keeps an SPS30 inside
its `-10 °C..+60 °C` operating range and keeps condensation off its optics
through a winter.

The control loop runs on `ProcessingTask` at 1 Hz and forces the output off
whenever its temperature probe goes stale, the enclosure exceeds the
over-temperature cutoff, or the module is disabled. Duty ramps from zero on
every on-edge so a cold PTC's inrush does not trip the supply.

The software interlocks do not replace the hardware ones: the gate still needs
a pull-down so the heater is off while the ESP32 is in reset, and the element
still needs an in-line thermal fuse.

### Exporters

Toggled via `#ifdef EXPORT_*_ENABLED` in `src/setup.h`. All five are enabled by default.

- **MQTT** (`EXPORT_MQTT_ENABLED`)
- **Generic HTTP POST** (`EXPORT_HTTP_ENABLED`)
- **sensor.community** (`EXPORT_SENSORCOMMUNITY_ENABLED`)
- **openSenseMap** (`EXPORT_OPENSENSEMAP_ENABLED`)
- **Webhook** — Discord / Slack / IFTTT (`EXPORT_WEBHOOK_ENABLED`)

### Operating modes

Selected via `PlatformMode` enum in `src/core/Config.h`:

- **`legacy`** — deep-sleep water logger; FF/PF button triggers, RTC RAM log buffer
- **`continuous`** — FreeRTOS sensor/processing/storage/export pipeline, no deep sleep
- **`hybrid`** — legacy water logger + FreeRTOS pipeline running concurrently

### Web UI

On-device single-page app served from LittleFS (no internet required):

- **Dashboard & log charts** — uPlot time-series; `uPlot.*` is vendored in `www/` so charts render in offline / AP-only mode (CDN is fallback only)
- **Module manager** — schema-driven settings for Wi-Fi, time/NTP, data log, theme, OTA and the enclosure heater, each with a live status chip and enable toggle (`/api/modules`)
- **Sensors** — add/edit/calibrate/reorder with pin-conflict guards; live read-now test
- **OTA, diagnostics, file browser** — rollback-capable updates, `/api/diag` observability, and CSV/log download
- First-run wizard, dark/light themes, CSRF-protected mutating endpoints

---

## Supported boards

| Board id | Display name | Max GPIO | Notes |
|---|---|---|---|
| `xiao_c3` | Seeed XIAO ESP32-C3 | 21 | Strap: 2, 8, 9. Flash: 11–17. USB CDC: 18, 19. UART0: 20, 21 |
| `supermini_c3` | ESP32-C3 SuperMini | 21 | Same restrictions as XIAO C3; USB CDC on boot (`-DARDUINO_USB_CDC_ON_BOOT=1`) |
| `generic_c3` | Generic ESP32-C3 | 21 | Same chip constraints; USB CDC may or may not be enabled |
| `generic_s3` | Generic ESP32-S3 | 48 | Strap: 0, 3, 45, 46. Flash: 26–37 (octal). USB CDC: 19, 20. UART0: 43, 44 |
| `custom` | Custom — full responsibility | 48 | No pin validation; user accepts all restrictions |

Source: `src/core/BoardProfiles.cpp` profile definitions.

---

## Quick start

**1. Clone the repo**

```bash
git clone https://github.com/n3tk0/esp32_logger.git
cd esp32_logger
```

**2. Build firmware** — choose one environment from `platformio.ini`:

```bash
pio run -e xiao_esp32c3        # Seeed XIAO ESP32-C3 (default)
pio run -e esp32c3_supermini   # ESP32-C3 SuperMini
pio run -e lolin_c3_pico       # WEMOS LOLIN C3 PICO (4 MB, LOLIN I2C on GPIO 8/10)
pio run -e esp32s3             # ESP32-S3 DevKitC-1 (8 MB flash, no PSRAM)
pio run -e xiao_esp32s3        # Seeed XIAO ESP32-S3 (8 MB flash, 8 MB PSRAM)
pio run -e esp32s3_n16r8       # ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)
```

On the XIAO ESP32-S3 the PSRAM ring buffer is active out of the box — the Seeed
board definition already supplies `-DBOARD_HAS_PSRAM` and `qio_opi`, unlike the
generic DevKitC-1 profile that `esp32s3_n16r8` has to configure by hand. Pick
the **Seeed XIAO ESP32-S3** board profile in the first-run wizard: the header
breaks out only GPIO 1–9 and 43/44, and the profile is what stops the pin
pickers from offering the GPIOs that never leave the module.

**3. Upload firmware and LittleFS image**

```bash
pio run -e xiao_esp32c3 --target upload
pio run -e xiao_esp32c3 --target uploadfs
```

`tools/build_web.py` must be run first to produce the LittleFS image from `www/` (minifies and gzip-compresses JS/CSS into `dist/www/`).

**4. Connect to the device AP**

| Setting | Default |
|---|---|
| SSID | `WaterLogger` |
| Password | `water12345` |

Defined in `DEFAULT_AP_SSID` / `DEFAULT_AP_PASSWORD` in `src/core/Config.h`.

**5. Open the first-run wizard**

Navigate to `http://192.168.4.1/firstrun` (AP-mode IP). Select a board profile, choose an operating mode, and assign GPIO pins. The device reboots with the saved configuration.

---

## Project structure

```
ESP32_Logger/
├── ESP_Logger.ino          # Arduino entry point
├── platformio.ini          # Build environments for all 3 boards
├── partitions_balanced.csv # OTA-capable 4 MB partition table
│
├── src/                    # Firmware source
│   ├── core/               # Config, board profiles, module registry
│   ├── managers/           # Storage, Wi-Fi, OTA, RTC, hardware
│   ├── modules/            # Pluggable runtime modules
│   ├── sensors/            # Sensor plugins + manager
│   ├── pipeline/           # Aggregation + flow-run logger
│   ├── tasks/              # FreeRTOS tasks
│   ├── export/             # MQTT/HTTP/community exporters
│   ├── web/                # HTTP API + auth + CSRF
│   └── ...
│
├── www/                    # Web UI (HTML/CSS/JS, served from LittleFS)
├── tools/                  # Build, flash, deploy, and provisioning scripts
├── scripts/                # PlatformIO pre-build hooks
├── schematics/             # Hardware schematics and reference designs
├── docs/                   # Architecture, audit log, operating guide
└── .github/workflows/      # CI: firmware + bootloader builds
```

---

## Documentation

Full documentation lives in [`docs/`](docs/):

- **[INSTRUCTIONS.md](docs/INSTRUCTIONS.md)** — operating the device after first boot (sensors, exporters, OTA, safe mode, diagnostics, troubleshooting)
- **[ARCHITECTURE.md](docs/ARCHITECTURE.md)** — module layout, task model, data pipeline
- **[REFACTORING_GUIDELINES.md](docs/REFACTORING_GUIDELINES.md)** — architecture invariants and SOPs for code changes
- **[AUDIT_LOG.md](docs/AUDIT_LOG.md)** — security/architecture audit findings (R1–R20 + post-R20 rounds: WebUI #150, backend #151, web-auth/CSRF #152)
- **[FUTURE_UPDATES.md](docs/FUTURE_UPDATES.md)** — roadmap
- **[USB_CDC_IMPLEMENTATION_SUMMARY.md](docs/USB_CDC_IMPLEMENTATION_SUMMARY.md)** — USB CDC toggle implementation

---

## Hardware

Reference schematics (KiCad PDFs) for the water-flow build are in [`schematics/`](schematics/):

- `Flow_meter_diagram_rev.1.pdf` – `rev.3.pdf` — revision history
- [`schematics/README.md`](schematics/README.md) — wiring notes

---

## Contributing

Issues and pull requests are welcome. Before opening a PR:

1. Read [`docs/REFACTORING_GUIDELINES.md`](docs/REFACTORING_GUIDELINES.md) for architectural invariants.
2. Make sure the firmware still builds for all six CI targets (`xiao_esp32c3`, `esp32c3_supermini`, `lolin_c3_pico`, `esp32s3`, `xiao_esp32s3`, `esp32s3_n16r8`) — the workflows in `.github/workflows/` run automatically on PRs.
3. Keep changes scoped; large refactors should be split.

---

## License

See [LICENSE](LICENSE).

---

<div align="center">

## Support the project

If ESP32 Logger is useful to you, consider supporting development.
Every contribution helps cover hardware, certifications, and time spent maintaining the project.

[![Donate via Revolut](https://img.shields.io/badge/Donate-Revolut-0075EB?style=for-the-badge&logo=revolut&logoColor=white)](https://revolut.me/petk0g)

Thank you for your support.

</div>
