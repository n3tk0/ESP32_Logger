# Future Updates Checklist

Track planned improvements and known gaps here. Check off items when completed.

---

## WebUI — Sensor Management

- [x] Sensor Add popup with type-selection button grid (replaces prompt)
- [x] Sensor Edit popup with per-interface form fields (replaces JSON prompt)
- [x] Pin dropdowns with greyed-out already-used pins
- [x] ZMPT101B (AC voltage) sensor type added
- [x] ZMCT103C (AC current) sensor type added
- [x] ADC-only pin filter toggle in the edit popup (checkbox to show all or ADC-only pins)
- [x] Show system/reserved pin warnings (⚠ label suffix on reserved GPIOs in dropdown)
- [x] Sensor reorder (up/down arrows in the list)
- [x] Duplicate sensor button (copy existing config with new ID)
- [x] Sensor test/ping button (🔍 reads live value via /api/sensors/read_now)
- [x] Calibration sub-form (offset + scale per metric) inside the edit popup
- [x] Support remaining sensor types in the Add popup: soil_moisture, ds18b20, bme688, hcsr04, bh1750, veml6075, veml7700, scd4x

---

## WebUI — Dashboard / Charts

- [x] Zoom & pan on charts (🔍+/🔍− buttons + reset; index-based view window)
- [ ] Multi-metric overlay: plot two sensors on the same chart with dual Y-axes
- [x] Export chart as PNG/SVG
- [x] Persistent chart configuration (filters saved/restored via localStorage)
- [x] Live page: configurable refresh rate
- [x] Alert/threshold markers on charts (horizontal lines at user-defined levels)

---

## WebUI — General

- [x] Keyboard shortcut reference page (? key)
- [x] Toast/snackbar notifications instead of inline message divs
- [x] Confirm dialog before discarding unsaved changes on settings pages
- [x] Settings search / filter (filter cards by title on the settings hub)
- [x] Dark/light theme toggle in header (quick toggle, not just in settings)

---

## Firmware — Sensor Plugins

- [x] Implement ZMPT101B plugin (RMS voltage via ADC sampling)
- [x] Implement ZMCT103C plugin (RMS current via ADC sampling, burden resistor support)
- [x] Register ZMPT101B + ZMCT103C in Logger.ino plugin factory
- [x] Add soil_moisture sensor to platform_config.json example
- [x] DS18B20 multi-sensor support (up to 8 sensors on one OneWire bus)
- [x] VEML7700 lux calibration support (CalibrationAxis _calLux + _calWhite)

---

## Firmware — Core / Platform

- [x] Watchdog recovery log (record last-reset cause to LittleFS /reset_log.txt)
- [x] Per-sensor error counter exposed in /api/sensors (error_count field)
- [x] Dynamic sensor reload without full restart (/api/config/platform POST)
- [ ] OTA rollback support (keep previous firmware partition, revert on crash)
- [x] MQTT discovery payload for Home Assistant auto-discovery (ha_discovery flag)
- [x] Sensor data webhook (HTTP POST on threshold breach — WebhookExporter with rules)

---

## Failsafe Page

- [x] Add Sensor button in Core Logic tab
- [x] Edit (JSON prompt) and Remove buttons per sensor row
- [x] Pin conflict warning in failsafe edit (lightweight, no dropdown needed)
- [x] Show firmware version and LittleFS usage in failsafe header

---

## Infrastructure

- [x] Build script to gzip www/ assets before flashing (tools/gzip_www.sh)
- [x] Automated integration test against mock /api endpoints (tests/test_api.sh)
- [x] Changelog auto-generated from git log on release (tools/generate_changelog.sh)

---

_Last updated: 2026-04-09_
