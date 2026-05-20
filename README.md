# ESP32 Logger

Multi-sensor environmental logging platform for embedded ESP32 targets.
Ships firmware for three boards: `xiao_esp32c3`, `esp32c3_supermini`, `esp32s3`
(see `platformio.ini`).

---

## Features

**Sensors** (toggled via `#ifdef SENSOR_*_ENABLED` in `src/setup.h`):

| Macro | Sensor | Interface |
|---|---|---|
| `SENSOR_BME280_ENABLED` | BME280/BMP280 temp/humidity/pressure | I2C |
| `SENSOR_BME688_ENABLED` | BME680/688 + gas resistance | I2C |
| `SENSOR_DS18B20_ENABLED` | DS18B20 temperature (1-Wire) | GPIO |
| `SENSOR_SDS011_ENABLED` | SDS011 PM2.5/PM10 | UART |
| `SENSOR_PMS5003_ENABLED` | PMS5003 PM1/2.5/10 | UART |
| `SENSOR_ENS160_ENABLED` | ENS160 TVOC/eCO2/AQI | I2C |
| `SENSOR_SGP30_ENABLED` | SGP30 TVOC/eCO2 | I2C |
| `SENSOR_SCD4X_ENABLED` | SCD40/41 CO2/temp/humidity | I2C |
| `SENSOR_VEML6075_ENABLED` | VEML6075 UV-A/B/index | I2C |
| `SENSOR_VEML7700_ENABLED` | VEML7700 ambient light | I2C |
| `SENSOR_BH1750_ENABLED` | BH1750 lux | I2C |
| `SENSOR_WATERFLOW_ENABLED` | YF-S201/YF-S403 water flow | GPIO/ISR |
| `SENSOR_RAIN_ENABLED` | Tipping-bucket rain gauge | GPIO/ISR |
| `SENSOR_WIND_ENABLED` | Anemometer + wind vane | GPIO/ADC |
| `SENSOR_SOIL_ENABLED` | Capacitive soil moisture | ADC |
| `SENSOR_HCSR04_ENABLED` | HC-SR04 ultrasonic distance | GPIO |
| `SENSOR_ZMPT101B_ENABLED` | ZMPT101B AC voltage | ADC |
| `SENSOR_ZMCT103C_ENABLED` | ZMCT103C AC current | ADC |

Default build (`src/setup.h`) enables only `SENSOR_BME280_ENABLED` and
`SENSOR_SDS011_ENABLED`; all others are commented out.

**Exporters** (toggled via `#ifdef EXPORT_*_ENABLED` in `src/setup.h`):
- MQTT (`EXPORT_MQTT_ENABLED`)
- Generic HTTP POST (`EXPORT_HTTP_ENABLED`)
- sensor.community (`EXPORT_SENSORCOMMUNITY_ENABLED`)
- openSenseMap (`EXPORT_OPENSENSEMAP_ENABLED`)
- Webhook — Discord/Slack/IFTTT (`EXPORT_WEBHOOK_ENABLED`)

All five are enabled by default.

**Operating modes** (`PlatformMode` enum in `src/core/Config.h`):
- `legacy` — deep-sleep water logger; FF/PF button triggers, RTC RAM log buffer
- `continuous` — FreeRTOS sensor/processing/storage/export pipeline, no deep sleep
- `hybrid` — legacy water logger + FreeRTOS pipeline running concurrently

---

## Supported boards

| Board id | Display name | Max GPIO | Notes |
|---|---|---|---|
| `xiao_c3` | Seeed XIAO ESP32-C3 | 21 | Strap: 2,8,9. Flash: 11–17. USB CDC: 18,19. UART0: 20,21 |
| `supermini_c3` | ESP32-C3 SuperMini | 21 | Same restrictions as XIAO C3; USB CDC on boot (`-DARDUINO_USB_CDC_ON_BOOT=1`) |
| `generic_c3` | Generic ESP32-C3 | 21 | Same chip constraints; USB CDC may or may not be enabled |
| `generic_s3` | Generic ESP32-S3 | 48 | Strap: 0,3,45,46. Flash: 26–37 (octal). USB CDC: 19,20. UART0: 43,44 |
| `custom` | Custom — full responsibility | 48 | No pin validation; user accepts all restrictions |

Source: `src/core/BoardProfiles.cpp` profile definitions.

---

## Quick start

1. **Clone the repo**

   ```bash
   git clone https://github.com/n3tk0/esp32_logger.git
   cd esp32_logger
   ```

2. **Build firmware** — choose one environment from `platformio.ini`:

   ```bash
   pio run -e xiao_esp32c3        # Seeed XIAO ESP32-C3 (default)
   pio run -e esp32c3_supermini   # ESP32-C3 SuperMini
   pio run -e esp32s3             # ESP32-S3 DevKitC-1 (8 MB)
   ```

3. **Upload firmware and LittleFS image**

   ```bash
   pio run -e xiao_esp32c3 --target upload
   pio run -e xiao_esp32c3 --target uploadfs
   ```

   `tools/build_web.py` must be run first to produce the LittleFS image from
   `www/` (minifies and gzip-compresses JS/CSS into `dist/www/`).

4. **Connect to the device AP**

   Default SSID: `WaterLogger`  
   Default password: `water12345`  
   (Source: `DEFAULT_AP_SSID` / `DEFAULT_AP_PASSWORD` in `src/core/Config.h`)

5. **Open the first-run wizard**

   Navigate to `http://192.168.4.1/firstrun` (AP-mode IP).  
   Select a board profile, choose an operating mode, and assign GPIO pins.
   The device reboots with the saved configuration.

---

## Documentation

- [INSTRUCTIONS.md](INSTRUCTIONS.md) — operating the device after first boot
- [REFACTORING_GUIDELINES.md](REFACTORING_GUIDELINES.md) — architecture
  invariants and SOPs for code changes
- [AUDIT_LOG.md](AUDIT_LOG.md) — security/architecture audit findings (R1–R20)

---

## License

See LICENSE.
