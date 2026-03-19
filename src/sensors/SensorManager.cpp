#include "SensorManager.h"
#include <LittleFS.h>

SensorManager sensorManager;

// ---------------------------------------------------------------------------
bool SensorManager::registerPlugin(const char* type, SensorFactory factory) {
    if (_pluginCount >= MAX_PLUGINS) return false;
    strncpy(_plugins[_pluginCount].type, type, sizeof(_plugins[0].type) - 1);
    _plugins[_pluginCount].factory = factory;
    _pluginCount++;
    return true;
}

// ---------------------------------------------------------------------------
ISensor* SensorManager::_createPlugin(const char* type) {
    for (int i = 0; i < _pluginCount; i++) {
        if (strcmp(_plugins[i].type, type) == 0) {
            return _plugins[i].factory();
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
void SensorManager::_destroyAll() {
    for (int i = 0; i < _count; i++) {
        delete _sensors[i];
        _sensors[i] = nullptr;
    }
    _count = 0;
    memset(_lastReadMs, 0, sizeof(_lastReadMs));
}

// ---------------------------------------------------------------------------
bool SensorManager::loadAndInit(fs::FS& fs, const char* cfgPath) {
    _destroyAll();

    File f = fs.open(cfgPath, FILE_READ);
    if (!f) {
        Serial.printf("[SensorManager] %s not found\n", cfgPath);
        return false;
    }

    // Use a static doc to avoid heap fragmentation — 4KB is plenty for config
    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[SensorManager] JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray arr = doc["sensors"].as<JsonArray>();
    if (arr.isNull()) {
        Serial.println("[SensorManager] No 'sensors' array in config");
        return false;
    }

    int initialised = 0;
    for (JsonObject sensor : arr) {
        if (!sensor["enabled"] | false) continue;
        if (_count >= MAX_SENSORS) {
            Serial.println("[SensorManager] MAX_SENSORS reached");
            break;
        }

        const char* type = sensor["type"] | "";
        const char* id   = sensor["id"]   | type;

        ISensor* s = _createPlugin(type);
        if (!s) {
            Serial.printf("[SensorManager] Unknown plugin type: %s\n", type);
            continue;
        }

        s->setId(id);
        if (s->init(sensor)) {
            _sensors[_count]    = s;
            _lastReadMs[_count] = 0;
            _count++;
            initialised++;
            Serial.printf("[SensorManager] Sensor '%s' (%s) ready\n", id, type);
        } else {
            Serial.printf("[SensorManager] Sensor '%s' init FAILED\n", id);
            delete s;
        }
    }

    Serial.printf("[SensorManager] %d/%d sensors initialised\n",
                  initialised, _count);
    return initialised > 0;
}

// ---------------------------------------------------------------------------
int SensorManager::tick(QueueHandle_t sensorQueue, uint32_t now) {
    int pushed = 0;
    uint32_t ms = millis();

    // Up to 4 readings per sensor per tick (multi-metric sensors)
    SensorReading readings[4];

    for (int i = 0; i < _count; i++) {
        ISensor* s = _sensors[i];
        if (!s || !s->isEnabled()) continue;

        uint32_t intervalMs = s->getReadIntervalMs();
        if (intervalMs > 0 && (ms - _lastReadMs[i]) < intervalMs) continue;

        int n = s->readAll(readings, 4);
        if (n > 0) {
            for (int j = 0; j < n; j++) {
                readings[j].timestamp = now;
                strncpy(readings[j].sensorId,   s->getId(),   sizeof(readings[j].sensorId)   - 1);
                strncpy(readings[j].sensorType, s->getType(), sizeof(readings[j].sensorType) - 1);

                if (xQueueSend(sensorQueue, &readings[j], 0) == pdTRUE) {
                    pushed++;
                }
            }
            _lastReadMs[i] = ms;
        }
    }
    return pushed;
}

// ---------------------------------------------------------------------------
bool SensorManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}

// ---------------------------------------------------------------------------
ISensor* SensorManager::get(int index) {
    if (index < 0 || index >= _count) return nullptr;
    return _sensors[index];
}

// ---------------------------------------------------------------------------
ISensor* SensorManager::getById(const char* id) {
    for (int i = 0; i < _count; i++) {
        if (_sensors[i] && strcmp(_sensors[i]->getId(), id) == 0) {
            return _sensors[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
void SensorManager::toJson(JsonArray arr) const {
    for (int i = 0; i < _count; i++) {
        ISensor* s = _sensors[i];
        if (!s) continue;
        JsonObject o = arr.createNestedObject();
        o["id"]           = s->getId();
        o["type"]         = s->getType();
        o["name"]         = s->getName();
        o["enabled"]      = s->isEnabled();
        o["last_read_ts"] = s->lastReadTs();
        o["status"]       = s->isEnabled() ? "ok" : "disabled";
        JsonArray metrics = o.createNestedArray("metrics");
        const char* mNames[8];
        int mCount = s->getMetrics(mNames, 8);
        for (int m = 0; m < mCount; m++) metrics.add(mNames[m]);
    }
}
