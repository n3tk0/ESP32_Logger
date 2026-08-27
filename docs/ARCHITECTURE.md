# ESP32 Environmental Sensing Platform — Architecture v5.0

> Evolution of Water Logger v4.2.0 → Multi-Sensor Environmental Platform
> Target: XIAO ESP32-C3 (single-core RISC-V, primary) + ESP32 dual-core (optional)

---

## 1. System Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        HARDWARE LAYER                                   │
│  SDS011  PMS5003  BME280  ENS160  YF-S201  Rain  Wind  DS1302  SD/FS   │
└────────────┬──────────────────────────────────────────────┬────────────┘
             │ UART/I2C/SPI/Pulse/Analog                    │ SPI/I2C
             ▼                                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                       SENSOR PLUGIN LAYER                               │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐    │
│  │SDS011    │ │BME280    │ │PMS5003   │ │YF-S201   │ │ENS160    │    │
│  │Sensor    │ │Sensor    │ │Sensor    │ │Sensor    │ │Sensor    │    │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘    │
│  ┌──────────┐ ┌──────────┐                                             │
│  │Rain      │ │Wind      │   All implement ISensor interface           │
│  │Sensor    │ │Sensor    │                                             │
│  └──────────┘ └──────────┘                                             │
└─────────────────────┬───────────────────────────────────────────────────┘
                      │ SensorReading structs
                      ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    FREERTOS TASK PIPELINE                               │
│                                                                         │
│  ┌───────────────┐    sensorQ     ┌───────────────────┐                │
│  │  SensorTask   │───────────────►│  ProcessingTask   │                │
│  │  (prio 3)     │                │  (prio 2)         │                │
│  │               │                │  normalize        │                │
│  │  - tick all   │                │  validate         │                │
│  │    sensors    │                │  aggregate        │                │
│  │  - ISR pulses │                │  (LTTB/AVG/       │                │
│  │  - UART read  │                │   MIN/MAX/RAW)    │                │
│  └───────────────┘                └────────┬──────────┘                │
│                                            │                           │
│                              ┌─────────────┼──────────────┐            │
│                              │             │              │            │
│                              ▼             ▼              ▼            │
│                    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ │
│                    │ StorageTask  │ │  ExportTask  │ │   WebTask    │ │
│                    │ (prio 1)     │ │  (prio 1)    │ │ (AsyncWS)   │ │
│                    │             │ │              │ │              │ │
│                    │ JSON lines  │ │ MQTT         │ │ /api/data    │ │
│                    │ SD+LittleFS │ │ HTTP POST    │ │ Chart.js     │ │
│                    │ rotation    │ │ Sensor.Comm  │ │ SPA          │ │
│                    └──────────────┘ │ openSenseMap │ └──────────────┘ │
│                                     └──────────────┘                  │
└─────────────────────────────────────────────────────────────────────────┘
                                            │
                                            ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      STORAGE LAYER                                      │
│                                                                         │
│  SD Card present?                                                       │
│  ├── YES: /logs/YYYY-MM-DD.jsonl  (full history on SD)                 │
│  │        LittleFS used as cache / fast-read layer                     │
│  └── NO:  /logs/YYYY-MM-DD.jsonl  (LittleFS, auto-rotating)           │
│                                                                         │
│  Always:                                                                │
│  /config.bin              — binary device config (v12, unchanged)      │
│  /platform_config.json    — NEW: sensor + export + aggregation config  │
│  /logs/YYYY-MM-DD.jsonl   — JSON lines sensor data (raw, immutable)    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Folder / File Structure

```
Water_logger/
├── Logger.ino                          ← entry point (keep, minimal changes)
├── platformio.ini                      ← NEW: PlatformIO build config
├── ARCHITECTURE.md                     ← this file
├── docs/
│   ├── MIGRATION_PLAN.md
│   └── API_REFERENCE.md
│
├── www/                                ← existing SPA (extend only)
│   ├── index.html                      ← add "Core Logic" + "Datalog" pages
│   ├── web.js                          ← extend with new API calls
│   ├── style.css
│   └── chart.min.js
│
└── src/
    ├── core/
    │   ├── Config.h                    ← existing (UNCHANGED)
    │   ├── Globals.h/.cpp              ← existing (extend minimally)
    │   └── SensorTypes.h              ← NEW: SensorReading, enums, units
    │
    ├── managers/                       ← ALL existing (UNCHANGED)
    │   ├── ConfigManager.h/.cpp
    │   ├── HardwareManager.h/.cpp
    │   ├── StorageManager.h/.cpp
    │   ├── DataLogger.h/.cpp
    │   ├── WiFiManager.h/.cpp
    │   └── RtcManager.h/.cpp
    │
    ├── sensors/
    │   ├── ISensor.h                  ← NEW: pure virtual interface
    │   ├── SensorManager.h/.cpp       ← NEW: plugin registry
    │   ├── SensorConfig.h             ← NEW: per-sensor JSON config
    │   └── plugins/
    │       ├── BME280Sensor.h/.cpp    ← NEW: temp/humidity/pressure (I2C)
    │       ├── SDS011Sensor.h/.cpp    ← NEW: PM2.5/PM10 (UART)
    │       ├── PMS5003Sensor.h/.cpp   ← NEW: PM2.5/PM10 (UART)
    │       ├── YFS201Sensor.h/.cpp    ← NEW: water flow (pulse)
    │       ├── ENS160Sensor.h/.cpp    ← NEW: TVOC/eCO2 (I2C)
    │       ├── SGP30Sensor.h/.cpp     ← NEW: TVOC/eCO2 (I2C)
    │       ├── RainSensor.h/.cpp      ← NEW: rain gauge (pulse+time)
    │       └── WindSensor.h/.cpp      ← NEW: anemometer (pulse freq)
    │
    ├── pipeline/
    │   ├── DataPipeline.h/.cpp        ← NEW: queue init, routing
    │   ├── AggregationEngine.h/.cpp   ← NEW: LTTB + time buckets
    │   └── Normalizer.h/.cpp          ← NEW: unit conversion, spike filter
    │
    ├── storage/
    │   ├── JsonLogger.h/.cpp          ← NEW: JSON lines writer
    │   └── HybridStorage.h/.cpp       ← NEW: SD+LittleFS routing
    │
    ├── export/
    │   ├── IExporter.h                ← NEW: exporter interface
    │   ├── ExportManager.h/.cpp       ← NEW: registry + scheduling
    │   ├── MqttExporter.h/.cpp        ← NEW: MQTT (PubSubClient)
    │   ├── HttpExporter.h/.cpp        ← NEW: generic HTTP POST
    │   ├── SensorCommunityExporter.h/.cpp
    │   └── OpenSenseMapExporter.h/.cpp
    │
    ├── tasks/
    │   ├── TaskManager.h/.cpp         ← NEW: xTaskCreate + queue handles
    │   ├── SensorTask.h/.cpp          ← NEW: sensor tick loop
    │   ├── ProcessingTask.h/.cpp      ← NEW: normalize + aggregate
    │   ├── StorageTask.h/.cpp         ← NEW: write JSON lines
    │   └── ExportTask.h/.cpp          ← NEW: dispatch to exporters
    │
    └── web/
        ├── WebServer.h/.cpp           ← existing (add new routes only)
        └── ApiHandlers.h/.cpp         ← NEW: /api/data + /api/sensors
```

---

## 3. Class Design

### 3.1 Core Types (`src/core/SensorTypes.h`)

```cpp
enum SensorQuality : uint8_t {
    QUALITY_UNKNOWN   = 0,
    QUALITY_GOOD      = 1,
    QUALITY_ESTIMATED = 2,
    QUALITY_ERROR     = 3,
};

struct SensorReading {
    uint32_t timestamp;      // Unix epoch seconds
    char     sensorId[17];   // unique sensor instance id, e.g. "env_indoor"
    char     sensorType[12]; // plugin type: "bme280", "sds011", "yfs201"
    char     metric[16];     // "temperature", "pm25", "flow_rate", etc.
    float    value;
    char     unit[12];       // "C", "ug/m3", "L/min", "hPa", "%", etc.
    SensorQuality quality;
};

// JSON lines representation:
// {"ts":1710000000,"id":"env_indoor","sensor":"bme280",
//  "metric":"temperature","value":21.4,"unit":"C","q":1}
```

**These arrays are hard limits, and overrunning one is silent.**
`SensorReading::make()` copies with `strncpy(dst, src, sizeof(dst) - 1)`, so a
metric name of 16 characters is stored as 15 with no error at compile time or
run time — and every later `strcmp()` against the full name then misses, which
makes the metric look present (the plugin advertises it, MQTT discovery
publishes it) while nothing ever reads it. `humidity_ambient` was exactly that
and never worked. Budget **15 characters for `metric`**, 11 for `unit` and
`sensorType`, 16 for `sensorId`; `tools/check_metric_names.py` (run in the
`api-docs` CI job) fails the build on a literal that would truncate.

### 3.2 Sensor Interface (`src/sensors/ISensor.h`)

```cpp
class ISensor {
public:
    virtual ~ISensor() = default;

    // Called once; config is the sensor's JSON object from platform_config.json
    virtual bool        init(JsonObjectConst config)    = 0;

    // Fill `out` and return true on valid reading
    virtual bool        read(SensorReading& out)        = 0;

    // For multi-metric sensors: fill array, return count
    virtual int         readAll(SensorReading* out, int maxOut) {
        if (read(out[0])) return 1;
        return 0;
    }

    virtual const char* getType()            const = 0;
    virtual const char* getName()            const = 0;
    virtual uint32_t    getReadIntervalMs()  const { return 5000; }
    virtual bool        isEnabled()          const { return _enabled; }

protected:
    bool     _enabled = false;
    char     _id[17]  = {};
};

using SensorFactory = ISensor* (*)();
```

### 3.3 SensorManager (`src/sensors/SensorManager.h`)

```cpp
class SensorManager {
public:
    // Register plugin factory at compile time
    void registerPlugin(const char* type, SensorFactory factory);

    // Load platform_config.json, instantiate & init enabled sensors
    bool loadAndInit(fs::FS& fs, const char* path = "/platform_config.json");

    // Called from SensorTask every iteration
    // Returns number of readings pushed to queue
    int tick(QueueHandle_t sensorQueue, uint32_t now);

    // Reload config at runtime (web UI "save sensors")
    bool reloadConfig(fs::FS& fs);

    int     count()          const;
    ISensor* get(int index);
    ISensor* getById(const char* id);

private:
    static constexpr int MAX_SENSORS = 16;
    ISensor*     _sensors[MAX_SENSORS] = {};
    int          _count = 0;

    struct PluginEntry {
        char           type[16];
        SensorFactory  factory;
    };
    static constexpr int MAX_PLUGINS = 16;
    PluginEntry    _plugins[MAX_PLUGINS];
    int            _pluginCount = 0;

    uint32_t       _lastRead[MAX_SENSORS] = {};
};
```

### 3.4 Aggregation Engine (`src/pipeline/AggregationEngine.h`)

```cpp
enum AggMode : uint8_t {
    AGG_RAW  = 0,
    AGG_AVG  = 1,
    AGG_MIN  = 2,
    AGG_MAX  = 3,
    AGG_LTTB = 4,  // DEFAULT
    AGG_SUM  = 5,
};

enum TimeBucket : uint16_t {
    BUCKET_RAW   = 0,
    BUCKET_1MIN  = 1,
    BUCKET_5MIN  = 5,
    BUCKET_1HOUR = 60,
    BUCKET_1DAY  = 1440,
};

class AggregationEngine {
public:
    // LTTB: reduce inLen points to maxPoints preserving visual shape
    // Returns actual output count
    static size_t lttb(const SensorReading* in, size_t inLen,
                       SensorReading* out, size_t maxPoints);

    // Bucket readings into time windows, apply aggMode per bucket
    static size_t bucket(const SensorReading* in, size_t inLen,
                         SensorReading* out, size_t outMaxLen,
                         TimeBucket bucketMins, AggMode mode);

    // Combined pipeline: bucket → LTTB if still over maxPoints
    static size_t aggregate(const SensorReading* in, size_t inLen,
                            SensorReading* out, size_t outMaxLen,
                            TimeBucket bucketMins, AggMode mode,
                            size_t maxPoints = 500);
};
```

### 3.5 Exporter Interface (`src/export/IExporter.h`)

```cpp
class IExporter {
public:
    virtual ~IExporter() = default;

    virtual bool        init(JsonObjectConst config)                      = 0;
    virtual bool        send(const SensorReading* readings, size_t count) = 0;
    virtual const char* getName()     const = 0;
    virtual bool        isEnabled()   const = 0;
    virtual uint8_t     maxRetries()  const { return 3; }
    virtual uint32_t    retryDelayMs()const { return 5000; }
};

using ExporterFactory = IExporter* (*)();
```

### 3.6 FreeRTOS Task Architecture

```cpp
// src/tasks/TaskManager.h — queue handles (extern, shared across tasks)
extern QueueHandle_t sensorQueue;    // SensorReading, depth 20
extern QueueHandle_t storageQueue;   // SensorReading, depth 32
extern QueueHandle_t exportQueue;    // SensorReading, depth 32
extern SemaphoreHandle_t webDataMutex;   // guards ring buffer
extern SemaphoreHandle_t configMutex;    // guards config reload

// Task priorities
#define TASK_PRIO_SENSOR   3
#define TASK_PRIO_PROCESS  2
#define TASK_PRIO_STORAGE  1
#define TASK_PRIO_EXPORT   1

// Stack sizes (bytes) — tuned for ESP32-C3 (38KB DRAM)
#define STACK_SENSOR    4096
#define STACK_PROCESS   6144   // LTTB needs local buffer
#define STACK_STORAGE   4096
#define STACK_EXPORT    8192   // WiFi + TLS overhead
```

---

## 4. Data Pipeline Detail

```
ISensor::readAll()
    │
    │  SensorReading[] (raw)
    ▼
sensorQueue
    │
    ▼
ProcessingTask
    ├── Normalizer (unit conversion, range check, 3σ spike filter)
    ├── HeaterModule::tick()  (MODULE_HEATER_ENABLED, 1 Hz)
    │     Runs BEFORE the queue receive so it also fires on the 100 ms
    │     timeout path — the stale-probe fail-safe has to keep working
    │     precisely when no readings are arriving. Reads its control
    │     inputs from ReadingCache, not from the queue.
    ├── In-memory ring buffer (lock-free SPSC, runtime capacity:
    │     16 KB internal SRAM, or up to 4 MB in PSRAM when present —
    │     see webRingBufInit(). Currently the only store of recent
    │     readings, since the FS query path is not wired up yet.
    │     /api/latest and sparklines read its newest end directly;
    │     /api/data still caps a request at 300 raw readings.
    │     Backward scans are bounded by RING_SCAN_LIMIT_* so a metric
    │     missing from a large ring cannot stall webDataMutex.)
    │                   │
    │              webDataMutex  ←── WebTask reads for /api/data
    │
    ├──► storageQueue ──► StorageTask ──► JsonLogger
    │                                      ├── SD:  /logs/YYYY-MM-DD.jsonl
    │                                      └── LFS: /logs/YYYY-MM-DD.jsonl
    │
    └──► exportQueue  ──► ExportTask  ──► ExportManager
                                            ├── MqttExporter
                                            ├── HttpExporter
                                            ├── SensorCommunityExporter
                                            └── OpenSenseMapExporter

INVARIANT: Raw data written to JsonLogger is NEVER modified.
           Aggregation is read-time only (API + visualization).
```

---

## 5. Config System Extension

Backward-compatible strategy: keep `/config.bin` at v12 **unchanged**.
New functionality configured via `/platform_config.json` (JSON, human-editable).

### `/platform_config.json` Schema

```json
{
  "version": 1,
  "sensors": [
    {
      "id": "flow_main",
      "type": "yfs201",
      "enabled": true,
      "interface": "pulse",
      "pin": 21,
      "pulses_per_liter": 450.0,
      "calibration": 1.0,
      "read_interval_ms": 1000
    },
    {
      "id": "env_indoor",
      "type": "bme280",
      "enabled": true,
      "interface": "i2c",
      "sda": 6,
      "scl": 7,
      "address": 118,
      "read_interval_ms": 10000
    },
    {
      "id": "dust_outdoor",
      "type": "sds011",
      "enabled": false,
      "interface": "uart",
      "uart_rx": 20,
      "uart_tx": -1,
      "baud": 9600,
      "work_period_min": 1
    },
    {
      "id": "rain_gauge",
      "type": "rain",
      "enabled": false,
      "interface": "pulse",
      "pin": 9,
      "mm_per_pulse": 0.2794
    },
    {
      "id": "wind_speed",
      "type": "wind",
      "enabled": false,
      "interface": "pulse",
      "pin": 8,
      "sample_window_ms": 3000
    }
  ],
  "aggregation": {
    "default_mode": "lttb",
    "default_bucket_min": 5,
    "max_points": 500,
    "raw_retention_days": 7,
    "ring_buffer_size": 1000
  },
  "export": {
    "mqtt": {
      "enabled": false,
      "broker": "192.168.1.100",
      "port": 1883,
      "topic_prefix": "waterlogger",
      "client_id": "",
      "username": "",
      "password": "",
      "qos": 0,
      "retain": false,
      "interval_ms": 60000
    },
    "http": {
      "enabled": false,
      "url": "http://192.168.1.100:8086/api/v2/write",
      "method": "POST",
      "headers": {},
      "interval_ms": 60000
    },
    "sensor_community": {
      "enabled": false,
      "interval_ms": 145000
    },
    "opensensemap": {
      "enabled": false,
      "box_id": "",
      "access_token": "",
      "sensor_ids": {}
    }
  },
  "storage": {
    "log_dir": "/logs",
    "rotate_daily": true,
    "max_file_size_kb": 512,
    "cloud_only": false
  }
}
```

---

## 6. Local API Reference

> **This section is verified in CI.** `tools/check_api_docs.py` extracts every
> route registered in `src/web/` and fails the build if any is missing from the
> endpoint index below. When you add a `server.on(...)` route, add a row here
> (or allow-list it in the script if it is a captive-portal/SPA route).

### 6.1 Authentication & CSRF

Read-only (`GET`) endpoints are open on the local network. **Mutating routes go
through `requireMutatingAuth()` = rate-limit + CSRF.** The CSRF token is a
per-boot 128-bit value from `GET /api/csrf-token`, supplied as a **`?csrf=`
query parameter** (`CsrfToken::require()` reads the token from a form field *or*
the query string, so JSON-body routes are covered too). The SPA's
`postWithCsrf()` / `settingsSave()` helpers attach it automatically; the PROGMEM
failsafe page fetches it before each mutating call.

`WEB_BASIC_AUTH_ENABLED` (compile-time) is an **orthogonal** layer — HTTP Basic
Auth enforced by a handler gate registered first — and applies whether or not it
is compiled in; the CSRF/rate-limit layer above is always present.

The **Auth** column in the index uses:

| Tag | Meaning |
|---|---|
| `read` | Read-only; no CSRF (a few side-effecting GETs are marked `CSRF`) |
| `CSRF` | `requireMutatingAuth` — rate-limit **and** a valid `?csrf=` token |
| `first-run` | Allowed only while `g_setupRequired` (provisioning); 403 afterwards |

### 6.2 Endpoint index

**System & status**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/status` | read | Full snapshot (identity + runtime + theme) |
| GET | `/api/identity` | read | Device id / name / board / firmware |
| GET | `/api/runtime` | read | Heap, uptime, boot count, FS usage |
| GET | `/api/live` | read | Live legacy water-cycle snapshot (polled) |
| GET | `/api/diag` | read | Diagnostics / observability blob (R19) |
| GET | `/api/recent_logs` | read | Tail of the in-RAM log ring |
| GET | `/api/changelog` | read | Bundled `changelog.txt` |
| GET | `/api/csrf-token` | read | Issues the per-boot CSRF token |
| GET | `/api/theme` | read | Current theme tokens |

**Sensors & data**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/sensors` | read | Registered sensors + status |
| GET | `/api/sensors/read_now` | read | Force a synchronous read of all sensors |
| GET | `/api/latest` | read | Latest value per sensor/metric |
| GET | `/api/data` | read | Aggregated time-series (params in §6.3) |
| GET | `/api/backup` | read | Download a full config/data backup |
| POST | `/api/ingest` | token | Accept readings pushed by a remote node (`FEATURE_REMOTE_NODES`) |
| GET | `/kindle` | read | Server-rendered e-ink dashboard (`FEATURE_KINDLE_DASHBOARD`) |
| GET | `/kindle/probe` | read | Reports the reader's viewport, DPR and user agent, to pick `KINDLE_PAGE_W` |
| GET | `/kindle/clear` | read | Full-screen black/white frames to clear e-ink ghosting, then back to `/kindle` |

`/api/ingest` is the one mutating route that does **not** go through
`requireMutatingAuth()`. That chain checks a CSRF token, which exists to stop a
browser being made to issue a state-changing request on the strength of
credentials it carries automatically. A sensor node has no cookie jar, no
session and no origin, so there is nothing for CSRF to protect and no way for
the node to obtain a token. It is gated by the shared `INGEST_TOKEN` (header
`X-Ingest-Token` or `?token=`), the same rate limiter as every other mutating
route, and whatever Basic Auth is compiled in globally.

**Alerts**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/alerts` | read | Alert rules + engine state |
| GET | `/api/alerts/toasts` | read | Pending alert toasts |
| POST | `/api/alerts` | CSRF | Replace the whole alert-rule document |
| POST | `/api/alerts/snooze` | CSRF | Snooze a firing alert |

**Modules**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/modules` | read | Module index (status chips) |
| GET | `/api/modules/:id` | read | Module detail + config + schema |
| POST | `/api/modules/:id` | CSRF | Save `{enabled, config}` |
| POST | `/api/modules/:id/enable` | CSRF | Fast enable/disable (`?on=1`) |
| POST | `/api/modules/:id/restart` | CSRF | `stop()` + `start()` without changing enable |
| GET | `/api/modules/wifi/scan` | read | Cached Wi-Fi scan results |
| GET | `/api/modules/wifi/test` | read | Poll a Wi-Fi credential-test result (rate-limited) |
| POST | `/api/modules/wifi/test` | CSRF | Start a Wi-Fi credential test |
| GET | `/wifi_scan_start` | CSRF | Kick off an async Wi-Fi scan (side-effecting GET) |
| GET | `/wifi_scan_result` | read | Poll async Wi-Fi scan results |

**ESP-NOW battery nodes** (`FEATURE_ESPNOW_INGEST`)

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/espnow/status` | read | Nodes, battery, last seen, and the drop counters |
| POST | `/api/espnow/pair` | CSRF | Open a pairing window (`seconds`, 30–600) |
| POST | `/api/espnow/node` | CSRF | Rename a node (`label`) or change its `interval` |
| POST | `/api/espnow/forget` | CSRF | Drop a node's radio peer and its slot |

The counters in `/api/espnow/status` are not decoration. "No readings are
arriving" has several very different causes — a mismatched key, a moved
channel, an unprovisioned node, frames arriving faster than the tick drains
them — and from the outside they look identical. `malformed`,
`unknown_node`, `replayed`, `ring_full` and `discover_bad_sig` are what tell
them apart.

Two fields are deliberately `null` rather than zero. `rssi` is unavailable on
Arduino core 2.x (IDF 4.4 hands the receive callback no signal information),
and `days` is null whenever `batteryDaysLeft()` refuses to answer — too little
history, or a slope inside the noise. A zero in either would read as a
measurement.

**Platform & settings**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/platform_config` | read | Raw `platform_config.json` |
| POST | `/api/config/platform` | CSRF | Reload sensors + exporters from config |
| POST | `/api/platform_reload` | CSRF | Trigger live reload + restart |
| POST | `/save_platform` | CSRF | Write `platform_config.json` (streamed JSON body) |
| POST | `/save_device` | CSRF | Device name / id / storage view |
| POST | `/save_hardware` | CSRF | Pin map / hardware config |
| POST | `/save_network` | CSRF | Wi-Fi / AP / hostname |
| POST | `/save_time` | CSRF | NTP / timezone / DST |
| POST | `/save_theme` | CSRF | Theme / accent / density / chart source |
| POST | `/save_datalog` | CSRF | Flow-log rotation / retention / format |
| POST | `/save_sensorlog` | CSRF | Sensor CSV logging config |
| POST | `/api/next-id` | CSRF | Generate a device id from the MAC |
| POST | `/api/regen-id` | CSRF | Legacy alias of `/api/next-id` |
| GET | `/export_settings` | read | Download all settings as JSON |
| POST | `/import_settings` | CSRF | Restore settings from an uploaded JSON |
| POST | `/api/mqtt/ha_discovery` | CSRF | Publish Home Assistant MQTT discovery |

**Time**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| POST | `/set_time` | CSRF | Set the RTC clock manually |
| POST | `/sync_time` | CSRF | Trigger an NTP sync |
| GET | `/api/time_sync_status` | read | Poll the NTP sync result |
| POST | `/rtc_protect` | CSRF | Toggle RTC write-protect |

**Data log & boot counter**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| POST | `/api/datalog/create` | CSRF | Create a new datalog file |
| POST | `/api/datalog/switch` | CSRF | Switch the active datalog file |
| POST | `/flush_logs` | CSRF | Flush the RAM log buffer to the FS |
| POST | `/backup_bootcount` | CSRF | Persist the boot counter to the FS |
| POST | `/restore_bootcount` | CSRF | Restore the boot counter from the FS |

**Files & storage**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/filelist` | read | List a directory (sanitized `dir` param) |
| GET | `/download` | read | Download a file |
| POST | `/delete` | CSRF | Delete a file |
| POST | `/mkdir` | CSRF | Create a directory |
| POST | `/move_file` | CSRF | Move / rename a file |
| POST | `/upload` | CSRF | Upload a file (multipart) |
| POST | `/api/format_filesystem` | CSRF | Format LittleFS (safe-mode recovery) |
| POST | `/factory_reset` | CSRF | Wipe config + reboot |

**OTA & firmware**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| GET | `/api/ota/status` | read | Running/previous partition + rollback state |
| POST | `/api/ota/confirm` | CSRF | Mark the current firmware stable |
| POST | `/api/ota/rollback` | CSRF | Revert to the previous partition + reboot |
| POST | `/do_update` | CSRF | Upload a firmware `.bin` |
| POST | `/restart` | CSRF | Reboot the device |

**Diagnostics & provisioning**

| Method | Route | Auth | Purpose |
|---|---|---|---|
| POST | `/api/i2c_scan` | CSRF | Scan an I2C bus (`?bus=N`, default 0; the bus must already be configured by a sensor) |
| GET | `/firstrun` | read | Serve the first-run wizard HTML |
| GET | `/api/board-profiles` | read | Available board profiles |
| POST | `/api/firstrun` | first-run | Provision board / pins / mode (only while `g_setupRequired`) |

### 6.3 Selected payloads

**`GET /api/data`** — aggregated time-series query.

| Param  | Values                          | Default |
|--------|---------------------------------|---------|
| from   | Unix timestamp                  | now-24h |
| to     | Unix timestamp                  | now     |
| sensor | sensor id (e.g. `env_indoor`)   | all     |
| metric | metric name (e.g. `pm25`)       | all     |
| agg    | raw, 1m, 5m, 1h, 1d             | 5m      |
| mode   | raw, avg, min, max, lttb        | lttb    |
| limit  | 1–5000                          | 500     |

```json
{
  "from": 1710000000, "to": 1710086400, "agg": "5m", "mode": "lttb",
  "sensor": "env_indoor", "metric": "temperature", "count": 287,
  "data": [
    {"ts": 1710000000, "v": 20.1},
    {"ts": 1710000300, "v": 20.4}
  ]
}
```

**`GET /api/sensors`**

```json
{
  "sensors": [
    {
      "id": "env_indoor", "type": "bme280", "name": "BME280 Environmental",
      "enabled": true, "last_read_ts": 1710086380,
      "metrics": ["temperature","humidity","pressure","dew_point","humidity_amb"], "status": "ok"
    }
  ]
}
```

`metrics` is what the sensor actually emits with its current configuration, not
a fixed catalogue for the type: `humidity_amb` appears only when a self-heating
correction is configured (`ambient_temp_sensor`, or a temperature calibration
that moves the number). Without one it is dropped, because it would be a
bit-for-bit copy of `humidity` — see §BME280/BME688 self-heating.

**`GET /api/modules`** — schema-driven module manager. Each registered `IModule`
(`wifi`, `ota`, `theme`, `datalog`, `time`) is listed with a description, a live
status chip and an enable toggle.

```json
[
  {
    "id": "wifi", "name": "Wi-Fi", "enabled": true, "hasUI": true,
    "description": "Station/AP connection, credentials and static-IP settings.",
    "status": { "text": "MyNet · 192.168.1.20 · -58 dBm", "tone": "ok" }
  }
]
```

`description` and `status` are optional (a module omits `status` when it has no
live signal; a disabled module returns none). `tone` ∈ `ok | warn | err | dim`.
Both come from the `IModule` hooks `getDescription()` and `statusJson(JsonObject)`;
`statusJson()` must be cheap and non-blocking — it runs on the AsyncTCP worker,
once per module per request (no FS scans, no network round-trips).

**`GET /api/modules/:id`** adds the per-module `config` object plus the PROGMEM
`schema` string that drives the settings form:

```json
{
  "id": "time", "name": "Time", "enabled": true, "hasUI": true,
  "config": { "ntpServer": "pool.ntp.org", "timezone": 1, "dstOffsetHours": 0 },
  "schema": "{\"fields\":[ … ]}"
}
```

Schema field keys: `id`, `type` (`string`/`int`/`float`/`bool`/`enum`/`color`/
`password`/`ipv4`), `label`, `min`/`max`/`step`, `unit`, `group`, `help`,
`showIf` (`"otherField"` or `{"field":value}`), and `options` (required for
`enum` — `{"v":<value>,"l":"<label>"}` objects; `v` is stored, `l` is displayed).

---

## 7. Export Formats

### MQTT Topics

```
{prefix}/device/{deviceId}/sensor/{sensorId}/{metric}
  Payload: {"ts":1710000000,"value":21.4,"unit":"C","q":1}

{prefix}/device/{deviceId}/status
  Payload: {"uptime":3600,"heap":45000,"rssi":-67,"ts":1710086400}
```

### Sensor.Community

```
POST https://api.sensor.community/v1/push-sensor-data/
X-Pin: 1   (SDS011=1, BME280=11, SHT31=7)
X-Sensor: esp32-{deviceId}
Content-Type: application/json

{
  "software_version": "WaterLogger v5.0",
  "sensordatavalues": [
    {"value_type":"P1","value":"18.1"},
    {"value_type":"P2","value":"12.4"}
  ]
}
```

### openSenseMap

```
POST https://api.opensensemap.org/boxes/{boxId}/data
Authorization: Bearer {access_token}
Content-Type: application/json

[
  {"sensor":"SENSOR_ID_TEMP","value":"21.4"},
  {"sensor":"SENSOR_ID_HUM", "value":"58.2"}
]
```

---

## 8. Thread Safety

```
FreeRTOS queue (built-in thread safety):
  sensorQueue    SensorTask  → ProcessingTask
  storageQueue   ProcessingTask → StorageTask
  exportQueue    ProcessingTask → ExportTask

Mutexes:
  fsMutex        serializes ALL filesystem writes (see below) — the single
                 most safety-critical lock in the firmware
  webDataMutex   ProcessingTask (write) ↔ WebTask (read) ring buffer
  configMutex    Web /api/config/platform POST ↔ SensorTask reload
  wireMutex      ALL I2C buses (SensorManager reads ↔ /api/i2c_scan).
                 Deliberately one lock for both controllers rather than
                 one each: two buses really can run concurrently, but the
                 transfers are short and seconds apart, so the contention
                 is irrelevant and a single lock keeps the scan endpoint
                 and the sensor tasks trivially correct against each other.
  HeaterModule
  ::_hwMutex     LEDC attach/detach bookkeeping. tick() runs on
                 ProcessingTask while stop()/start() arrive on the AsyncTCP
                 worker via /api/modules/heater/{enable,restart}. Held only
                 around attach/detach — never around a blocking call. stop()
                 de-energises the gate BEFORE taking it, so the safety
                 guarantee never depends on acquiring a lock.

Spinlock (portMUX):
  ReadingCache   latest value per (sensorId, metric). Written from
                 SensorTask AND SlowSensorTask inside tickFiltered, read by
                 BME688Sensor (for its ambient-temperature reference) and
                 HeaterModule. Deliberately NOT a FreeRTOS mutex and
                 deliberately not configMutex: BME688Sensor reads the cache
                 from inside tickFiltered, which already holds configMutex,
                 so a second take of that non-recursive mutex would deadlock.

ISR shared state (existing pattern, unchanged):
  volatile pulseCount   — ISR writes, SensorTask reads
  volatile flowDetected — ISR sets, SensorTask clears
  Critical sections: noInterrupts() / interrupts()
```

### 8.1 `fsMutex` — filesystem write serialization

Every writer to LittleFS/SD takes `fsMutex` (via the RAII `MutexGuard`, or the
`atomicWrite(fs, path, …, fsMutex)` helper). This includes `CsvLogger`,
`FlowRunLogger`, `ConfigManager` (`saveConfig`/crash-recovery), `AlertEngine`
(`_save()`), `DataLogger`, the boot-counter backup, and the streamed
`/save_platform` upload. Concurrent unserialized writes can interleave a
`tmp` open + `rename` against a log append and corrupt the filesystem, so a new
FS writer **must** hold `fsMutex`.

**Lock-ordering rule (critical — violating it deadlocks the StorageTask):**

`fsMutex` is a **plain, non-recursive** FreeRTOS mutex (`xSemaphoreCreateMutex`).
A task that already holds it must **not** re-acquire it deeper in the call
stack — `xSemaphoreTake` on a non-recursive mutex you already own blocks until
the 2 s timeout and returns `pdFALSE`.

- `StorageTask` takes `fsMutex` once per tick, then calls `CsvLogger::appendRow`
  and `flowRunLog.tick()` **under that lock**. Their internal helpers
  (`CsvLogger::_rotate`, `FlowRunLogger::_closeRun` /`_enforceSizeRotation`)
  therefore run with the lock already held and **must not** re-take it. (Two
  such self-deadlocks were fixed in the #151 audit — they had silently disabled
  CSV rotation and dropped every flow run.)
- A helper reachable **both** with and without the lock held (e.g.
  `RtcManager::backupBootCount`, called from web handlers *and* from
  `DataLogger::flushLogBufferToFS` which already holds `fsMutex`) keeps its own
  `atomicWrite(…, fsMutex)`; the holding caller releases its guard
  (`MutexGuard::release()`) before invoking it. Do not make the callee
  lock-agnostic — fix it at the one call site that double-locks.
- `AlertEngine::_save()` runs on the AsyncTCP web task; it passes `fsMutex` to
  `atomicWrite` (previously `nullptr`, which raced StorageTask writes).

---

## 9. Time Handling

```
Priority:
  1. DS1302 RTC (valid + set)  →  authoritative source for timestamps
  2. NTP synced                →  write epoch to RTC (UTC), use for timestamps
  3. Neither                   →  relative from millis(), quality=ESTIMATED

Storage:  everything is stored in UTC — log timestamps, RTC contents and the
          /api/data epochs. After an NTP sync the RTC is written from
          gmtime_r(), so it always holds UTC regardless of the configured zone.
Display:  all human-facing paths convert with localtime_r(). The zone is set via
          configTime(timezone*3600, dstOffsetHours*3600, ntpServer).
NTP sync: on boot (if WiFi client) and then self-healing — while the link is up
          and the clock is not yet valid, loop() re-queues a sync every 60 s
          until one succeeds. Manual sync: POST /sync_time (CSRF) → poll
          /api/time_sync_status. A blank ntpServer falls back to
          DEFAULT_NTP_SERVER.
Format:   Unix epoch uint32_t (seconds since 1970-01-01 UTC)

Migration note: a device whose RTC was set by older (local-time) firmware shows
a doubled offset until the next NTP sync rewrites the RTC in UTC.
```

---

## 10. Operating Modes

| Mode            | Description                                    | Sleep  |
|-----------------|------------------------------------------------|--------|
| `legacy`        | Original water logger (deep sleep per event)   | YES    |
| `continuous`    | Multi-sensor polling loop (FreeRTOS tasks)     | NO     |
| `hybrid`        | Water logger + environmental monitoring        | NO     |

Mode selected via `platform_config.json` → `"mode": "continuous"`.
Default on first boot: `"legacy"` (preserves existing behavior).

---

## 11. Hardware Constraints (ESP32-C3 Specific)

### D2 — Deep Sleep GPIO Wakeup: C3-Only Implementation

`configureWakeup()` in `RtcManager.cpp` uses `esp_deep_sleep_enable_gpio_wakeup()`,
which is available **only on ESP32-C3 and ESP32-S2** and accepts a pin bitmask.

On the original **ESP32 (dual-core Xtensa)**, GPIO wakeup from deep sleep requires
`esp_sleep_enable_ext0_wakeup()` (single pin) or `esp_sleep_enable_ext1_wakeup()`
(multiple pins via RTC domain), with a different API and different capable-pin constraints.

**Supported wakeup pins on ESP32-C3:** GPIO 0–5 only (RTC-capable pins).

> **Portability note:** If porting to ESP32 (non-C3), the wakeup configuration in
> `configureWakeup()` must be rewritten. The rest of the firmware is compatible.

| Board            | Wakeup API                              | Wake-capable pins |
|------------------|-----------------------------------------|-------------------|
| XIAO ESP32-C3    | `esp_deep_sleep_enable_gpio_wakeup()`   | GPIO 0–5          |
| ESP32-C3 SuperMini | `esp_deep_sleep_enable_gpio_wakeup()` | GPIO 0–5          |
| ESP32 (Xtensa)   | `esp_sleep_enable_ext0/ext1_wakeup()`  | RTC GPIO 0, 2, 4, 12–15, 25–27, 32–39 |

---

## 12. Pin Assignment Guide

### P1 — RTC (DS1302) Pins vs I2C Sensor Bus

The DS1302 uses a **bit-banged 3-wire protocol** (CE, IO, SCLK) implemented via
the `ThreeWire` library — it does **not** use the hardware I2C peripheral.

However, by default the firmware assigns the RTC to GPIO 5 (CE), 6 (IO), 7 (SCLK).
GPIO 6 and 7 are also the **default `Wire` I2C bus** on ESP32-C3 (SDA=8, SCL=9 per
Arduino-ESP32 defaults, but many I2C sensor configs use 6/7).

**Risk:** If an I2C sensor (BME280, ENS160, etc.) is configured with `sda=6, scl=7`,
the RTC bit-banging and I2C communication will conflict on the same pins.

**Recommended pin assignment for XIAO ESP32-C3:**

| Function         | GPIO | Notes |
|------------------|------|-------|
| FF Wake button   | 3    | Deep-sleep wake capable |
| PF Wake button   | 4    | Deep-sleep wake capable |
| WiFi trigger     | 2    | Deep-sleep wake capable |
| DS1302 CE        | 5    | Deep-sleep wake capable, safe for bit-bang |
| DS1302 IO        | 20   | USB-CDC RX — **only use when USB CDC is disabled** |
| DS1302 SCLK      | 21   | Flow sensor default — reassign if using RTC here |
| I2C SDA          | 6    | Use when RTC IO is NOT on GPIO 6 |
| I2C SCL          | 7    | Use when RTC SCLK is NOT on GPIO 7 |
| Flow sensor      | 21   | ISR-capable, non-wake |
| SD CS            | 10   | Boot strapping — safe after boot |

**Recommended `platform_config.json` for I2C + RTC coexistence:**

```json
{
  "sensors": [
    {
      "type": "bme280",
      "sda": 6,
      "scl": 7
    }
  ]
}
```

In `config.bin` hardware settings, set RTC pins to avoid 6/7:
- `pinRtcCE = 5`, `pinRtcIO = 20`, `pinRtcSCLK = 21`

> **Warning:** GPIO 20 and 21 are USB-CDC RX/TX on ESP32-C3. They can be used
> as general GPIO only when the USB CDC Serial is disabled (i.e., when you use
> hardware UART or the board is powered without USB connection). If you rely on
> USB Serial for debugging, do not use GPIO 20/21 for RTC.

### Pin Safety Matrix

| GPIO | Wake? | I2C? | UART? | Boot-strap? | Recommended Use |
|------|-------|------|-------|-------------|-----------------|
| 0    | ✅    | —    | —     | ⚠️ pull-up  | Button / wake   |
| 1    | ✅    | —    | —     | —           | Button / wake   |
| 2    | ✅    | —    | —     | ⚠️ pull-up  | WiFi trigger    |
| 3    | ✅    | —    | —     | —           | FF button       |
| 4    | ✅    | —    | —     | —           | PF button       |
| 5    | ✅    | —    | —     | —           | RTC CE          |
| 6    | ❌    | SDA  | —     | —           | I2C sensors     |
| 7    | ❌    | SCL  | —     | —           | I2C sensors     |
| 8    | ❌    | SDA* | —     | ⚠️ pull-up  | Alt I2C / GPIO  |
| 9    | ❌    | SCL* | —     | ⚠️ pull-up  | Alt I2C / GPIO  |
| 10   | ❌    | —    | —     | ⚠️ pull-up  | SD CS           |
| 20   | ❌    | —    | RX    | —           | USB-CDC / RTC IO|
| 21   | ❌    | —    | TX    | —           | Flow sensor     |

*GPIO 8/9 are the Arduino-ESP32 `Wire` defaults but have internal pull-ups that
may conflict with some I2C slaves — use with care.
