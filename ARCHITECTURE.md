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
    ├── In-memory ring buffer per metric (last 1000 pts, lock-free SPSC)
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

### `GET /api/data`

| Param  | Values                           | Default |
|--------|----------------------------------|---------|
| from   | Unix timestamp                   | now-24h |
| to     | Unix timestamp                   | now     |
| sensor | sensor id (e.g. "env_indoor")   | all     |
| metric | metric name (e.g. "pm25")       | all     |
| agg    | raw, 1m, 5m, 1h, 1d             | 5m      |
| mode   | raw, avg, min, max, lttb         | lttb    |
| limit  | 1–5000                           | 500     |

**Example response:**

```json
{
  "from": 1710000000,
  "to":   1710086400,
  "agg":  "5m",
  "mode": "lttb",
  "sensor": "env_indoor",
  "metric": "temperature",
  "count": 287,
  "data": [
    {"ts": 1710000000, "v": 20.1},
    {"ts": 1710000300, "v": 20.4},
    {"ts": 1710000600, "v": 21.2}
  ]
}
```

### `GET /api/sensors`

```json
{
  "sensors": [
    {
      "id": "env_indoor",
      "type": "bme280",
      "name": "BME280 Environmental",
      "enabled": true,
      "last_read_ts": 1710086380,
      "metrics": ["temperature","humidity","pressure"],
      "status": "ok"
    },
    {
      "id": "flow_main",
      "type": "yfs201",
      "name": "YF-S201 Water Flow",
      "enabled": true,
      "last_read_ts": 1710086390,
      "metrics": ["flow_rate","volume"],
      "status": "ok"
    }
  ]
}
```

### `POST /api/config/platform`

Accepts updated `platform_config.json` body; reloads sensors & exporters live.

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
  webDataMutex   ProcessingTask (write) ↔ WebTask (read) ring buffer
  configMutex    Web /api/config/platform POST ↔ SensorTask reload

ISR shared state (existing pattern, unchanged):
  volatile pulseCount   — ISR writes, SensorTask reads
  volatile flowDetected — ISR sets, SensorTask clears
  Critical sections: noInterrupts() / interrupts()
```

---

## 9. Time Handling

```
Priority:
  1. DS1302 RTC (valid + set)  →  authoritative
  2. NTP synced                →  write to RTC, use for timestamps
  3. Neither                   →  relative from millis(), quality=ESTIMATED

NTP sync: on boot (if WiFi), then every 24h in continuous mode.
Format: Unix epoch uint32_t (seconds since 1970-01-01 UTC)
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
