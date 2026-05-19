#pragma once
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ISensor.h"

// ============================================================================
// Cross-plugin resource arbitration (R17 rows 21.1, 22.4).
//
// Plugins that share scarce hardware resources (Serial1, fixed I2C addresses)
// claim ownership before configuring the bus. The second plugin attempting
// to claim the same resource refuses to init, preventing silent collisions.
//
// Release helpers run from SensorManager's failure path so a plugin whose
// init() failed AFTER claiming a resource doesn't permanently block another
// plugin from claiming it. Same applies on reloadConfig() via _destroyAll().
//
// `who` is `ISensor::getType()` — a stable string literal per plugin type.
// ============================================================================
bool _claimSerial1(const char* who);
void _releaseSerial1(const char* who);

bool _claimI2cAddress(uint8_t addr, const char* who);
void _releaseI2cClaims(const char* who);

// ============================================================================
// SensorManager — plugin registry and tick dispatcher
//
// Usage:
//   SensorManager sensors;
//   sensors.registerPlugin("bme280",  []()->ISensor*{ return new BME280Sensor(); });
//   sensors.registerPlugin("sds011",  []()->ISensor*{ return new SDS011Sensor(); });
//   sensors.loadAndInit(LittleFS);   // reads /platform_config.json
//
//   // Inside SensorTask loop:
//   sensors.tick(sensorQueue, now);
// ============================================================================
class SensorManager {
public:
    static constexpr int MAX_SENSORS = 16;
    static constexpr int MAX_PLUGINS = 16;

    // ------------------------------------------------------------------
    // Plugin registration (call before loadAndInit)
    // ------------------------------------------------------------------
    bool registerPlugin(const char* type, SensorFactory factory);

    // ------------------------------------------------------------------
    // Load /platform_config.json, instantiate and init all enabled sensors
    // Returns true if at least one sensor initialised successfully
    // ------------------------------------------------------------------
    bool loadAndInit(fs::FS& fs,
                     const char* cfgPath = "/platform_config.json");

    // ------------------------------------------------------------------
    // tick()
    //   Called from SensorTask.  Iterates sensors whose read interval
    //   has elapsed and pushes readings to sensorQueue.
    //   `now` — current Unix timestamp (or millis()-based counter)
    //   Returns total readings pushed.
    // ------------------------------------------------------------------
    int tick(QueueHandle_t sensorQueue, uint32_t now);

    // ------------------------------------------------------------------
    // tickFiltered()
    //   Like tick() but only reads sensors where isBlocking() == blocking.
    //   SensorTask calls this with blocking=false (fast I2C/ISR sensors).
    //   SlowSensorTask calls this with blocking=true (UART/delay sensors).
    // ------------------------------------------------------------------
    int tickFiltered(QueueHandle_t queue, uint32_t now, bool blocking);

    // ------------------------------------------------------------------
    // Reload config at runtime (web UI config save)
    // Destroys old sensor instances, re-creates from updated JSON.
    // ------------------------------------------------------------------
    bool reloadConfig(fs::FS& fs,
                      const char* cfgPath = "/platform_config.json");

    // Accessors
    int      count()            const { return _count; }
    uint32_t minReadIntervalMs() const;   // smallest interval across all sensors (C1)
    ISensor* get(int index);
    ISensor* getById(const char* id);

    // Serialise current sensor status to JSON array for /api/sensors
    // Writes into `doc`; caller must ensure capacity.
    void     toJson(JsonArray arr) const;

private:
    struct PluginEntry {
        char          type[16];
        SensorFactory factory;
    };

    PluginEntry _plugins[MAX_PLUGINS];
    int         _pluginCount = 0;

    ISensor*    _sensors[MAX_SENSORS]   = {};
    uint32_t    _lastReadMs[MAX_SENSORS] = {};   // millis() of last read
    int         _count = 0;

    // ------------------------------------------------------------------
    // Per-sensor 24-hour health tracking.
    // Each HealthData holds 24 hourly buckets.  tickFiltered() writes to
    // the current bucket; toJson() reads and serialises all 24.
    // ------------------------------------------------------------------
    struct HealthData {
        uint32_t hourReads[24]  = {};   // successful reads per hour slot
        uint32_t hourErrors[24] = {};   // failed reads per hour slot
        uint32_t hourLatUs[24]  = {};   // accumulated latency (us) per slot
        uint8_t  curSlot        = 0;    // index of the current write slot (0-23)
        uint32_t slotStartMs    = 0;    // millis() when curSlot began (0 = unset)
        uint32_t totalLatUs     = 0;    // all-time latency accumulator
        uint32_t latSamples     = 0;    // all-time sample count
    };
    HealthData  _health[MAX_SENSORS];

    void _destroyAll();
    ISensor* _createPlugin(const char* type);
};

// Global singleton — defined in SensorManager.cpp
extern SensorManager sensorManager;
