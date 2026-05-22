# Migration Plan: Water Logger v4.2.0 → v5.0 Environmental Platform

---

## Overview

Migration is **non-destructive and backward compatible**.
Existing functionality (water flow logging, web UI, config) is preserved.
New capabilities are additive and controlled via `platform_config.json`.

---

## Phase 1 — Foundation (No Breaking Changes)

**Goal:** Add new files without touching existing ones.

### Steps

1. **Add new source files** to `src/`:
   - `core/SensorTypes.h`
   - `sensors/ISensor.h`, `SensorManager.h/.cpp`
   - `sensors/plugins/*.h/.cpp`
   - `pipeline/AggregationEngine.h/.cpp`, `DataPipeline.h/.cpp`
   - `storage/JsonLogger.h/.cpp`
   - `export/IExporter.h`, `ExportManager.h/.cpp`, `*Exporter.h/.cpp`
   - `tasks/TaskManager.h/.cpp`, `*Task.h/.cpp`
   - `web/ApiHandlers.h/.cpp`

2. **Add `platformio.ini`** — migrate from Arduino IDE.
   - Existing Arduino IDE workflow still works.

3. **Upload `platform_config.json`** to device LittleFS:
   - Default config has `"mode": "legacy"` → no behavior change.
   - Upload via web UI `/upload` or `pio run --target uploadfs`.

4. **Verify**: Flash + test existing water logging unchanged.

---

## Phase 2 — Activate Continuous Mode

**Goal:** Enable FreeRTOS task pipeline alongside existing logic.

### Changes to `Logger.ino`

```cpp
// Add at top of Logger.ino:
#include "src/sensors/plugins/BME280Sensor.h"
#include "src/sensors/plugins/SDS011Sensor.h"
#include "src/sensors/plugins/YFS201Sensor.h"
#include "src/sensors/plugins/RainSensor.h"
#include "src/sensors/plugins/WindSensor.h"
#include "src/sensors/SensorManager.h"
#include "src/pipeline/DataPipeline.h"
#include "src/tasks/TaskManager.h"
#include "src/export/ExportManager.h"
#include "src/export/MqttExporter.h"
#include "src/export/HttpExporter.h"
#include "src/export/SensorCommunityExporter.h"
#include "src/export/OpenSenseMapExporter.h"
#include "src/web/ApiHandlers.h"

// In setup(), after setupWebServer():
if (continuousMode) {
    // Register sensor plugins
    sensorManager.registerPlugin("bme280",  []()->ISensor*{ return new BME280Sensor(); });
    sensorManager.registerPlugin("sds011",  []()->ISensor*{ return new SDS011Sensor(); });
    sensorManager.registerPlugin("yfs201",  []()->ISensor*{ return new YFS201Sensor(); });
    sensorManager.registerPlugin("pms5003", []()->ISensor*{ return new PMS5003Sensor(); });
    sensorManager.registerPlugin("ens160",  []()->ISensor*{ return new ENS160Sensor(); });
    sensorManager.registerPlugin("rain",    []()->ISensor*{ return new RainSensor(); });
    sensorManager.registerPlugin("wind",    []()->ISensor*{ return new WindSensor(); });
    sensorManager.loadAndInit(*activeFS);

    // Register exporters
    exportManager.addExporter(new MqttExporter());
    exportManager.addExporter(new HttpExporter());
    exportManager.addExporter(new SensorCommunityExporter());
    exportManager.addExporter(new OpenSenseMapExporter());
    exportManager.loadAndInit(*activeFS);

    // Register new API routes
    // (server is the AsyncWebServer instance in WebServer.cpp)
    registerApiRoutes(server);

    // Start FreeRTOS tasks
    TaskManager::init(*activeFS);
}
```

### Mode Detection

```cpp
// Read mode from platform_config.json
bool continuousMode = false;
File pcfg = activeFS->open("/platform_config.json", FILE_READ);
if (pcfg) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, pcfg) == DeserializationError::Ok) {
        const char* mode = doc["mode"] | "legacy";
        continuousMode = (strcmp(mode, "continuous") == 0 ||
                          strcmp(mode, "hybrid")     == 0);
    }
    pcfg.close();
}
```

---

## Phase 3 — Enable Sensors

**Goal:** Add physical sensors; configure in `platform_config.json`.

### 3a — Add BME280 (temperature/humidity/pressure)

1. Wire BME280 to I2C (SDA=D4/GPIO6, SCL=D5/GPIO7 on XIAO ESP32-C3).
2. Edit `platform_config.json`:
   ```json
   {"id":"env_indoor","type":"bme280","enabled":true,"interface":"i2c",
    "sda":6,"scl":7,"address":118,"read_interval_ms":10000}
   ```
3. Change `"mode": "continuous"`.
4. Flash + verify readings in `/api/data?sensor=env_indoor`.

### 3b — Add SDS011 (PM2.5/PM10)

1. Wire RX to GPIO20 (TX not needed for read-only mode).
2. Enable in config: `"id":"dust_outdoor","type":"sds011","enabled":true,"uart_rx":20`.

### 3c — Migrate Water Flow to Plugin

The existing ISR-based flow logic can be phased out in favor of `YFS201Sensor`:
- `YFS201Sensor` uses the same ISR debounce pattern.
- Keep existing `DataLogger` for backward-compatible log format.
- `YFS201Sensor` additionally writes to JSON lines format.

### 3d — Add Rain / Wind

Wire tipping-bucket rain gauge and anemometer to spare GPIO pins.
Enable in config with appropriate `mm_per_pulse` / `meters_per_rev`.

---

## Phase 4 — Cloud Export

**Goal:** Send data to MQTT broker and/or cloud APIs.

### MQTT (Recommended)

1. Enable in `platform_config.json` → `export.mqtt.enabled = true`.
2. Set broker, port, topic_prefix.
3. Subscribe to `waterlogger/device/+/sensor/+/+` on broker.

### Sensor.Community

1. Register device at https://sensor.community/
2. Set `export.sensor_community.enabled = true`.
3. No additional setup — device ID auto-used as sensor ID.

### openSenseMap

1. Create a senseBox at https://opensensemap.org/
2. Copy box_id, access_token, sensor IDs into config.
3. Enable: `export.opensensemap.enabled = true`.

---

## Phase 5 — Web UI Extension

**Goal:** Add "Core Logic" and "Datalog" pages to existing SPA.

### New UI Pages

#### "Core Logic" page (`#corelogic`)

```html
<div class="sensor-list">
  <!-- Populated from GET /api/sensors -->
  <!-- Each sensor: toggle enable, show pin/type, last read time -->
</div>
```

#### "Datalog Settings" additions

Add to existing settings page:
- Aggregation interval dropdown: `raw | 1m | 5m | 1h | 1d`
- Aggregation mode: `lttb | avg | min | max`
- Max points: number input (1–5000)

#### Chart page extension

Update dashboard chart to call `/api/data` instead of (or in addition to) existing file-based data:

```javascript
// web.js addition
async function loadSensorChart(sensor, metric, from, to, agg, mode) {
    const r = await fetch(
        `/api/data?sensor=${sensor}&metric=${metric}`+
        `&from=${from}&to=${to}&agg=${agg}&mode=${mode}&limit=500`
    );
    const data = await r.json();
    // data.data = [{ts, v}, ...]
    renderChart(data.data);
}
```

---

## Config Migration Matrix

| Config Key              | Location              | Action          |
|-------------------------|-----------------------|-----------------|
| `config.bin` (v12)      | unchanged             | keep as-is      |
| FlowMeterConfig.pulsesPerLiter | `/config.bin` | keep for legacy |
| YFS201 pulses_per_liter | `/platform_config.json` | new (mirrors above) |
| New sensors             | `/platform_config.json` | new section     |
| MQTT settings           | `/platform_config.json` | new section     |
| Aggregation settings    | `/platform_config.json` | new section     |

---

## Risk Assessment

| Risk                         | Mitigation                                      |
|------------------------------|-------------------------------------------------|
| Config file missing          | SensorManager logs warning, continues in legacy |
| JSON parse error             | SensorManager returns false; legacy mode active |
| Sensor init failure          | Logged; other sensors continue                  |
| MQTT connection failure      | ExportManager retries 3x with backoff           |
| FreeRTOS stack overflow      | Stack sizes tuned; monitor with uxTaskGetStackHighWaterMark |
| ESP32-C3 single-core contention | All tasks on core 0; priorities set correctly |
| Deep sleep incompatibility   | Continuous mode disables deep sleep; legacy keeps it |

---

## Rollback

To roll back to v4.2.0 behavior:
1. Edit `platform_config.json` → `"mode": "legacy"`.
2. Flash firmware.
3. All existing functionality restored immediately.

Or: delete `/platform_config.json` — Logger.ino detects missing file and
stays in legacy mode without starting FreeRTOS tasks.

---

## Memory Budget (ESP32-C3, 400KB DRAM)

| Component               | Heap (approx)  |
|-------------------------|----------------|
| FreeRTOS overhead       | ~8 KB          |
| SensorTask stack        | 4 KB           |
| ProcessingTask stack    | 6 KB           |
| StorageTask stack       | 4 KB           |
| ExportTask stack        | 8 KB           |
| AsyncWebServer tasks    | ~12 KB         |
| WebRingBuf (500 items)  | ~40 KB         |
| ArduinoJson docs        | ~8 KB          |
| SensorManager (16 sensors) | ~2 KB       |
| **Total**               | **~92 KB**     |
| **Available (typical)** | **~180 KB**    |
| **Margin**              | **~88 KB**     |

> Note: WebRingBuf can be reduced to 200 items (16KB) if memory is tight.
