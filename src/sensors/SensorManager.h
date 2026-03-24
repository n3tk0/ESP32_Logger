#pragma once
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ISensor.h"

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

    void _destroyAll();
    ISensor* _createPlugin(const char* type);
};

// Global singleton — defined in SensorManager.cpp
extern SensorManager sensorManager;
