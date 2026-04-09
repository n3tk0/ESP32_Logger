# Future Updates Checklist

Track planned improvements and known gaps here. Check off items when completed.

---

## WebUI — Sensor Management

- [x] Sensor Add popup with type-selection button grid (replaces prompt)
- [x] Sensor Edit popup with per-interface form fields (replaces JSON prompt)
- [x] Pin dropdowns with greyed-out already-used pins
- [x] ZMPT101B (AC voltage) sensor type added
- [x] ZMCT103C (AC current) sensor type added
- [ ] ADC-only pin filter toggle in the edit popup (currently auto-filtered for analog interface)
- [ ] Show system/reserved pin warnings (e.g. WIFI_TRIGGER=2, RTC_IO=6, SD_CS=10)
- [ ] Sensor reorder (drag-and-drop or up/down arrows in the list)
- [ ] Duplicate sensor button (copy existing config with new ID)
- [ ] Sensor test/ping button that reads live value for a single sensor on demand
- [ ] Calibration sub-form (offset + scale per metric) inside the edit popup
- [ ] Support remaining sensor types in the Add popup: soil_moisture, ds18b20, bme688, hcsr04, bh1750, veml6075, veml7700, scd4x

---

## WebUI — Dashboard / Charts

- [ ] Zoom & pan on charts (pinch-to-zoom on mobile)
- [ ] Multi-metric overlay: plot two sensors on the same chart with dual Y-axes
- [ ] Export chart as PNG/SVG
- [ ] Persistent chart configuration (remember selected sensors/metrics across page loads)
- [ ] Live page: configurable refresh rate (currently fixed)
- [ ] Alert/threshold markers on charts (horizontal lines at user-defined levels)

---

## WebUI — General

- [ ] Keyboard shortcut reference page (? key)
- [ ] Toast/snackbar notifications instead of inline message divs
- [ ] Confirm dialog before discarding unsaved changes on settings pages
- [ ] Settings search / filter
- [ ] Dark/light theme toggle in header (quick toggle, not just in settings)

---

## Firmware — Sensor Plugins

- [ ] Implement ZMPT101B plugin (RMS voltage via ADC sampling, zero-crossing detection)
- [ ] Implement ZMCT103C plugin (RMS current via ADC sampling, burden resistor support)
- [ ] Register ZMPT101B + ZMCT103C in Logger.ino plugin factory
- [ ] Add soil_moisture sensor to platform_config.json example
- [ ] DS18B20 multi-sensor support (multiple sensors on one OneWire bus by address)
- [ ] VEML7700 lux calibration support

---

## Firmware — Core / Platform

- [ ] Watchdog recovery log (record last-reset cause to LittleFS)
- [ ] Per-sensor error counter exposed in /api/sensors
- [ ] Dynamic sensor reload without full restart (hot-reload platform config)
- [ ] OTA rollback support (keep previous firmware partition, revert on crash)
- [ ] MQTT discovery payload for Home Assistant auto-discovery
- [ ] Sensor data webhook (HTTP POST on threshold breach)

---

## Failsafe Page

- [x] Add Sensor button in Core Logic tab
- [x] Edit (JSON prompt) and Remove buttons per sensor row
- [ ] Pin conflict warning in failsafe edit (lightweight, no dropdown needed)
- [ ] Show firmware version and LittleFS usage in failsafe header

---

## Infrastructure

- [ ] Build script to gzip www/ assets before flashing (saves ~60% flash space)
- [ ] Automated integration test against mock /api endpoints
- [ ] Changelog auto-generated from git log on release

---

_Last updated: 2026-04-09_
