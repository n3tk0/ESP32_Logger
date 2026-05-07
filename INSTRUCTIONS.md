# ESP32 Logger — Build & Configuration Instructions

Complete reference for building, configuring, and flashing the firmware.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Building with Arduino IDE](#building-with-arduino-ide)
3. [Building with PlatformIO](#building-with-platformio)
4. [Module Toggles (Sensors & Exporters)](#module-toggles)
5. [Pin Definitions](#pin-definitions)
6. [FreeRTOS Task Tuning](#freertos-task-tuning)
7. [Partition Table & OTA](#partition-table--ota)
8. [Building a Rollback-Enabled Bootloader (ESP-IDF)](#building-a-rollback-enabled-bootloader)
9. [Flashing the Custom Bootloader](#flashing-the-custom-bootloader)
10. [Web UI Assets (LittleFS)](#web-ui-assets-littlefs)
11. [Runtime Configuration (platform_config.json)](#runtime-configuration)
12. [Build Size Optimization](#build-size-optimization)

---

## Prerequisites

**Hardware** (any of):
- XIAO ESP32-C3 (primary target)
- ESP32-C3 Super Mini (native USB — requires USB CDC On Boot = Enabled)
- Generic ESP32-DevKit

**Software** (one of):
- Arduino IDE 2.x + ESP32 Board Package 3.x
- PlatformIO (VS Code extension or CLI)

**Required libraries** (Arduino IDE — install via Library Manager):
| Library               | Author           | Version |
|-----------------------|------------------|---------|
| ArduinoJson           | Benoit Blanchon  | >= 7.0  |
| ESPAsyncWebServer     | esphome/lacamera | >= 3.1  |
| AsyncTCP              | me-no-dev        | >= 1.1  |

All sensor drivers, MQTT, and RTC use built-in mini drivers — no additional libraries are needed.

---

## Building with Arduino IDE

1. **Board selection**:
   - Tools → Board → ESP32 Arduino → **ESP32C3 Dev Module** (for Super Mini)
   - Tools → Board → ESP32 Arduino → **XIAO_ESP32C3** (for XIAO)
   - (or "ESP32 Dev Module" for generic ESP32)

2. **Super Mini specific settings**:
   - USB CDC On Boot: **Enabled** (required for serial monitor over native USB)
   - CPU Frequency: **160MHz**

3. **Partition scheme** — choose one:
   | Scheme | OTA | LittleFS | Notes |
   |--------|-----|----------|-------|
   | Huge APP (3MB/1MB SPIFFS) | No | 1 MB | Fits all modules, no OTA |
   | Minimal SPIFFS (1.9MB/190KB) | No | 190 KB | Tight — disable unused sensors |
   | Custom (`partitions_balanced.csv`) | Yes | 448 KB | **Recommended** — see [Partition Table](#partition-table--ota) |

3. **Upload speed**: 921600
4. **Monitor speed**: 115200

6. **Module toggles** — edit the `#define` block in `src/setup.h`. Comment out any sensor or exporter you don't use:
   ```cpp
   // #define SENSOR_BME280_ENABLED    // ← commented out = excluded from build
   #define SENSOR_DS18B20_ENABLED      // ← included
   ```

7. **Pin overrides** — edit `src/setup.h` (section 2) if your wiring differs from the defaults.

8. **Compile & upload** as usual (Ctrl+U).

> **⚠️ First flash with custom bootloader:** If you have already flashed a custom rollback bootloader via `flash_bootloader.py`, use **Sketch → Export compiled Binary** and then flash only the app with `esptool` at address `0x10000` to avoid overwriting the bootloader. See [Flashing the Custom Bootloader](#flashing-the-custom-bootloader).

---

## Building with PlatformIO

```bash
# Default build (XIAO ESP32-C3, all modules enabled)
pio run

# Generic ESP32
pio run -e esp32

# Upload firmware
pio run -t upload

# Upload LittleFS data (web UI)
pio run -t uploadfs

# Serial monitor
pio device monitor
```

All build-time options are controlled via `-D` flags in `platformio.ini` under `build_flags`. Comment out any `-DSENSOR_*` or `-DEXPORT_*` line to exclude that module.

---

## Module Toggles

Each module adds ~10–80 KB to the firmware binary. Only enable what your hardware uses.

### Sensors

| Define | Sensor | Interface | Flash cost |
|--------|--------|-----------|------------|
| `SENSOR_BME280_ENABLED` | BME280 / BMP280 | I2C | ~12 KB |
| `SENSOR_BME688_ENABLED` | BME680 / BME688 (gas) | I2C | ~15 KB |
| `SENSOR_DS18B20_ENABLED` | DS18B20 (up to 8 on one bus) | 1-Wire | ~10 KB |
| `SENSOR_SDS011_ENABLED` | SDS011 PM2.5/PM10 | UART | ~8 KB |
| `SENSOR_PMS5003_ENABLED` | PMS5003 particulate | UART | ~8 KB |
| `SENSOR_ENS160_ENABLED` | ENS160 VOC/eCO2 | I2C | ~7 KB |
| `SENSOR_SGP30_ENABLED` | SGP30 TVOC/eCO2 | I2C | ~7 KB |
| `SENSOR_SCD4X_ENABLED` | SCD40/SCD41 CO2 | I2C | ~8 KB |
| `SENSOR_VEML6075_ENABLED` | VEML6075 UV index | I2C | ~6 KB |
| `SENSOR_VEML7700_ENABLED` | VEML7700 ambient light | I2C | ~7 KB |
| `SENSOR_BH1750_ENABLED` | BH1750 light intensity | I2C | ~6 KB |
| `SENSOR_WATERFLOW_ENABLED` | YFS201/YFS403 flow meter | GPIO (ISR) | ~8 KB |
| `SENSOR_RAIN_ENABLED` | Tipping-bucket rain gauge | GPIO (ISR) | ~6 KB |
| `SENSOR_WIND_ENABLED` | Anemometer (pulse) | GPIO (ISR) | ~6 KB |
| `SENSOR_SOIL_ENABLED` | Capacitive soil moisture | ADC | ~5 KB |
| `SENSOR_HCSR04_ENABLED` | HC-SR04 ultrasonic distance | GPIO | ~5 KB |
| `SENSOR_ZMPT101B_ENABLED` | ZMPT101B AC voltage | ADC (RMS) | ~6 KB |
| `SENSOR_ZMCT103C_ENABLED` | ZMCT103C AC current | ADC (RMS) | ~6 KB |

### Exporters

| Define | Exporter | Flash cost |
|--------|----------|------------|
| `EXPORT_MQTT_ENABLED` | MQTT (built-in driver) | ~15 KB |
| `EXPORT_HTTP_ENABLED` | HTTP POST / webhook | ~10 KB |
| `EXPORT_SENSORCOMMUNITY_ENABLED` | Sensor.Community API | ~12 KB |
| `EXPORT_OPENSENSEMAP_ENABLED` | OpenSenseMap API | ~12 KB |

The Webhook exporter (`WebhookExporter`) is always compiled in (hardcoded `#define` in `Logger.ino`).

---

## Pin Definitions

Default pin mapping (edit in `src/setup.h`). Override in `platformio.ini` (`-DDEFAULT_SDA=X`).

| Define | XIAO ESP32-C3 | Super Mini | Function |
|--------|--------------|------------|----------|
| `DEFAULT_SDA` | 6 | 8 | I2C data |
| `DEFAULT_SCL` | 7 | 9 | I2C clock |
| `DEFAULT_FLOW_PIN` | 21 | 4 | Flow sensor input (ISR) |

> Current `setup.h` defaults are set for **ESP32-C3 Super Mini** (SDA=8, SCL=9, FLOW=4).

Additional hardware pins defined in `src/core/Config.h` → `DefaultPins` namespace:

| Pin | GPIO | Function |
|-----|------|----------|
| `WIFI_TRIGGER` | 2 | WiFi mode button |
| `WAKEUP_FF` | 3 | Full-flow wakeup button |
| `WAKEUP_PF` | 4 | Partial-flow wakeup button |
| `RTC_CE` | 5 | DS1302 chip enable |
| `RTC_IO` | 6 | DS1302 data |
| `RTC_SCLK` | 7 | DS1302 clock |
| `SD_CS` | 10 | SD card chip select |
| `SD_MOSI` | 11 | SD SPI MOSI |
| `SD_MISO` | 12 | SD SPI MISO |
| `SD_SCK` | 13 | SD SPI SCK |

> These defaults are compile-time fallbacks. Most pins can be overridden at runtime via the web UI or `platform_config.json`.

---

## FreeRTOS Task Tuning

Edit `src/tasks/TaskManager.h` to adjust. Reduce stack sizes to free DRAM if you disable modules.

### Task Priorities

| Task | Priority | Notes |
|------|----------|-------|
| SensorTask (fast) | 3 | Highest — I2C/ADC reads |
| ProcessTask | 2 | Aggregation + LTTB |
| SlowSensorTask | 2 | UART sensors (SDS011, PMS5003) |
| StorageTask | 1 | LittleFS / SD writes |
| ExportTask | 1 | WiFi + TLS network I/O |

### Stack Sizes

| Task | Default | Minimum safe | Why |
|------|---------|-------------|-----|
| `STACK_SENSOR_TASK` | 4096 | 3072 | I2C reads, small buffers |
| `STACK_PROCESS_TASK` | 6144 | 4096 | LTTB intermediate buffer on stack |
| `STACK_SLOW_SENSOR_TASK` | 4096 | 3072 | UART frame parsing |
| `STACK_STORAGE_TASK` | 6144 | 4096 | JsonLogger (~1.3 KB each) + file I/O |
| `STACK_EXPORT_TASK` | 8192 | 6144 | WiFi + TLS + JSON serialization |

### Queue Depths

| Queue | Default | Items × ~80 B |
|-------|---------|---------------|
| `QUEUE_SENSOR_DEPTH` | 20 | 1.6 KB |
| `QUEUE_STORAGE_DEPTH` | 32 | 2.5 KB |
| `QUEUE_EXPORT_DEPTH` | 32 | 2.5 KB |

---

## Partition Table & OTA

### Default: `partitions_balanced.csv` (4 MB flash)

```
Name      Type  SubType  Offset     Size
nvs       data  nvs      0x9000     0x5000    (20 KB)
otadata   data  ota      0xe000     0x2000    (8 KB)
app0      app   ota_0    0x10000    0x1C0000  (1,792 KB)
app1      app   ota_1    0x1D0000   0x1C0000  (1,792 KB)
spiffs    data  spiffs   0x390000   0x70000   (448 KB)
```

> **Note:** App partitions were increased from 1,536 KB to 1,792 KB in v5.1.0
> to accommodate the larger firmware produced by ESP32 Board Package 3.x.

- Two app slots (`ota_0`, `ota_1`) enable over-the-air updates
- `otadata` tracks which slot is active — required for OTA and rollback
- `spiffs` is used by LittleFS for web UI, config, and data logs

### Using the custom partition table in Arduino IDE

1. Copy `partitions_balanced.csv` to your Arduino sketchbook's `hardware/` directory, or place it alongside the `.ino` file.
2. In Arduino IDE: Tools → Partition Scheme → **Custom** (or select a built-in OTA-capable scheme).
3. Some ESP32 board packages require you to edit `boards.txt` to register the CSV. With PlatformIO this is handled by `board_build.partitions = partitions_balanced.csv`.

---

## Building a Rollback-Enabled Bootloader

> **Why?** The default Arduino/PlatformIO bootloader does **not** support automatic rollback-on-crash. Without a custom bootloader:
> - `OtaManager::isRollbackCapable()` returns `false`
> - Manual rollback via `/api/ota/rollback` still works
> - But the device will **not** auto-revert to the previous firmware if new firmware crashes during boot
>
> With a rollback-enabled bootloader:
> - After OTA, the new firmware is marked `PENDING_VERIFY`
> - If it crashes before calling `OtaManager::confirm()`, the bootloader automatically reverts
> - `OtaManager::isRollbackCapable()` returns `true`

### Easy path — Use pre-built binaries (recommended)

The GitHub Actions CI builds rollback-enabled bootloaders automatically. Pre-built binaries are committed to `tools/bootloader/esp32c3/`, `tools/bootloader/esp32c3_supermini/`, and `tools/bootloader/esp32/`.

```bash
# Install esptool if you don't have it
pip install esptool

# Flash the bootloader (auto-detects port and chip)
python tools/flash_bootloader.py

# Or specify explicitly
python tools/flash_bootloader.py --port /dev/ttyACM0 --chip esp32c3
python tools/flash_bootloader.py --port COM10 --chip esp32c3_supermini
python tools/flash_bootloader.py --port COM3 --chip esp32

# List available ports
python tools/flash_bootloader.py --list-ports
```

The tool flashes `bootloader.bin` and `partition-table.bin` to the correct addresses. It prompts for confirmation before writing.

After flashing the bootloader, upload your firmware as usual. The next OTA update will activate rollback-on-crash protection.

### Manual path — Build from source with ESP-IDF

Only needed if you want to customize bootloader settings beyond rollback support, or if pre-built binaries aren't available for your chip.

### Step 1 — Install ESP-IDF

Install ESP-IDF v5.1+ (must match the version used by your Arduino ESP32 core).

```bash
# Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.1.4   # match your Arduino core's IDF version

# Install toolchain
./install.sh esp32c3  # or esp32 for generic ESP32

# Activate environment
. ./export.sh
```

Check which IDF version your Arduino core uses:
```bash
# Arduino IDE: check the espressif/arduino-esp32 release notes
# PlatformIO:  pio pkg list | grep espressif32
```

### Step 2 — Create a bootloader project

```bash
# Start from the OTA example (it has the right sdkconfig defaults)
cp -r $IDF_PATH/examples/system/ota/simple_ota_example ~/bootloader_build
cd ~/bootloader_build
```

### Step 3 — Configure with menuconfig

```bash
idf.py set-target esp32c3   # or esp32
idf.py menuconfig
```

Navigate to these settings:

```
(Top) → Bootloader config
    [*] Enable app rollback support           ← ENABLE THIS
        (This sets CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)

(Top) → Bootloader config → Bootloader optimization level
    (Size)                                     ← Recommended (smaller bootloader)

(Top) → Partition Table
    (Custom partition table CSV)
    (partitions_balanced.csv)                  ← Point to your CSV
```

**Optional but recommended settings:**

```
(Top) → Bootloader config
    [*] GPIO triggers boot-mode select         ← Keep enabled
    [ ] Check SHA-256 on restart               ← Disable to save boot time
    (16) Log verbosity                         ← Set to Error or Warning

(Top) → Serial flasher config
    (921600) Flash baud rate
    (4 MB) Flash size

(Top) → Component config → ESP32C3-Specific
    (160) CPU frequency (MHz)

(Top) → Component config → FreeRTOS → Kernel
    [*] Run FreeRTOS only on first core        ← ESP32-C3 is single-core
```

### Step 4 — Build the bootloader only

```bash
idf.py bootloader
```

The output binary is at:
```
build/bootloader/bootloader.bin
```

### Step 5 — Copy the partition table

While you're at it, also build the matching partition table binary:
```bash
idf.py partition-table
```

Output: `build/partition_table/partition-table.bin`

---

## Flashing the Custom Bootloader

> **Warning**: Flashing a bad bootloader can brick the device. Keep a known-good bootloader.bin as backup. You can always recover via USB boot mode (hold BOOT button during reset on XIAO ESP32-C3).

### Option A — esptool.py (standalone)

```bash
# Flash bootloader only (does NOT erase app or data)
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0 bootloader.bin

# Flash bootloader + partition table together
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0    bootloader.bin \
    0x8000 partition-table.bin
```

Address map:
| Binary | Address | Notes |
|--------|---------|-------|
| `bootloader.bin` | `0x0` | Bootloader (16–32 KB) |
| `partition-table.bin` | `0x8000` | Partition table (3 KB) |
| `firmware.bin` (app) | `0x10000` | Your sketch (from Arduino IDE / PlatformIO) |

### Option B — PlatformIO custom bootloader

Add to `platformio.ini`:
```ini
board_build.bootloader = /path/to/your/bootloader.bin
```

Or flash manually alongside the normal upload:
```bash
pio run -t upload                              # upload firmware as usual
esptool.py ... write_flash 0x0 bootloader.bin  # then flash bootloader
```

### Option C — Arduino IDE

Use the ESP32 Sketch Data Upload tool or run `esptool.py` from the command line (Arduino IDE does not expose bootloader flashing in its GUI).

### Verifying rollback is active

After flashing the custom bootloader and performing an OTA update:

```bash
# Check via serial monitor
# Look for: [OTA] Firmware pending verify on app0 — auto-confirming

# Check via API
curl http://<device-ip>/api/ota/status
# Response: {"running_partition":"app0","previous_partition":"app1",
#            "pending_verify":false,"rollback_capable":true}
#                                                      ^^^^^^^^
```

If `rollback_capable` is `false`, the bootloader does not have rollback support compiled in.

---

## Web UI Assets (LittleFS)

The `www/` directory contains the web interface (HTML, JS, CSS). These must be uploaded to LittleFS.

### Pre-compress for space savings (~60%)

```bash
bash tools/gzip_www.sh
```

This creates `www_gz/` with `.gz` versions. The firmware's AsyncWebServer serves `.gz` files transparently.

### Upload to device

**PlatformIO:**
```bash
pio run -t uploadfs
```

**Arduino IDE:**
1. Copy contents of `www/` (or `www_gz/`) into a `data/` folder inside your sketch directory
2. Also copy `platform_config.json` into `data/`
3. Use Tools → ESP32 Sketch Data Upload (requires the LittleFS upload plugin)

---

## Runtime Configuration

`platform_config.json` is stored on LittleFS and loaded at boot. It controls sensors, exporters, aggregation, storage, and sleep behavior at runtime — no recompilation needed.

Edit via the web UI at `http://<device-ip>/settings` or modify the JSON directly.

Key sections:

### Sensors (runtime)
```json
{
  "sensors": [
    {
      "id": "bme280_1",
      "type": "bme280",
      "enabled": true,
      "address": "0x76",
      "sda": 6,
      "scl": 7,
      "read_interval_ms": 5000,
      "calibration": { "temperature": { "offset": -0.5, "scale": 1.0 } }
    }
  ]
}
```

### Aggregation
```json
{
  "aggregation": {
    "default_mode": "lttb",
    "default_bucket_min": 5,
    "max_points": 500,
    "raw_retention_days": 7,
    "ring_buffer_size": 500
  }
}
```

### Sleep modes
```json
{
  "sleep": {
    "continuous": {
      "idle_timeout_ms": 300000,
      "idle_cpu_mhz": 80,
      "modem_sleep": true
    },
    "hybrid": {
      "idle_before_sleep_ms": 120000,
      "sleep_duration_ms": 60000,
      "active_window_ms": 30000
    }
  }
}
```

### Export
```json
{
  "export": {
    "mqtt": {
      "enabled": true,
      "broker": "192.168.1.100",
      "port": 1883,
      "topic_prefix": "sensors/",
      "interval_ms": 60000,
      "ha_discovery": true
    }
  }
}
```

---

## Build Size Optimization

The ESP32-C3 has 4 MB flash. With OTA enabled, each app slot is 1.5 MB. A full build with all modules uses ~1.1–1.3 MB.

| Action | Savings |
|--------|---------|
| Disable unused sensors (comment out `#define`) | 5–80 KB each |
| Disable unused exporters | 10–20 KB each |
| Set `CORE_DEBUG_LEVEL=0` and `DEBUG_MODE=0` | ~3 KB |
| Pre-gzip web assets (`tools/gzip_www.sh`) | ~60% of www/ size |
| Reduce `STACK_EXPORT_TASK` if no TLS | 2 KB |
| Reduce queue depths if low sensor count | 1–3 KB DRAM |

To check current build size:
```bash
# PlatformIO
pio run -v 2>&1 | grep "Flash:"

# Arduino IDE
# Check the output pane after compilation for "Sketch uses X bytes"
```

---

## Quick Reference — All Build-Time Defines

| Define | Default | Where to set | Effect |
|--------|---------|-------------|--------|
| `CONFIG_FREERTOS_UNICORE` | 1 | platformio.ini / arduino_build_flags.h | Single-core FreeRTOS (ESP32-C3) |
| `CORE_DEBUG_LEVEL` | 0 | platformio.ini / arduino_build_flags.h | ESP-IDF log verbosity (0–5) |
| `DEBUG_MODE` | 0 | platformio.ini / Config.h | App serial debug output |
| `DEFAULT_SDA` | 8 | platformio.ini / setup.h | Default I2C SDA pin (6 for XIAO, 8 for Super Mini) |
| `DEFAULT_SCL` | 9 | platformio.ini / setup.h | Default I2C SCL pin (7 for XIAO, 9 for Super Mini) |
| `DEFAULT_FLOW_PIN` | 4 | platformio.ini / setup.h | Default flow sensor pin |
| `SENSOR_*_ENABLED` | all on | setup.h / platformio.ini | Include sensor module |
| `EXPORT_*_ENABLED` | all on | setup.h / platformio.ini | Include exporter module |

---

_Applies to firmware v5.1.0 — XIAO ESP32-C3 / ESP32-C3 Super Mini / generic ESP32_
