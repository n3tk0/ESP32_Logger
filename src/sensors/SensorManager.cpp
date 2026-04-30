#include "SensorManager.h"
#include <LittleFS.h>
#include "../pipeline/DataPipeline.h"  // wireMutex (#14)

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

    // Input-size cap (audit Pass 7 JsonDocument sizing): refuses oversize
    // sensor configs so a crafted file can't exhaust the heap during parse.
    constexpr size_t MAX_CFG_BYTES = 16 * 1024;
    if (f.size() > MAX_CFG_BYTES) {
        Serial.printf("[SensorManager] %s too large (%u B, cap %u)\n",
                      cfgPath, (unsigned)f.size(), (unsigned)MAX_CFG_BYTES);
        f.close();
        return false;
    }

    JsonDocument doc;
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
        if (!sensor["enabled"]) continue;
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
int SensorManager::tickFiltered(QueueHandle_t queue, uint32_t now, bool blocking) {
    int pushed = 0;
    uint32_t ms = millis();

    SensorReading readings[4];

    for (int i = 0; i < _count; i++) {
        ISensor* s = _sensors[i];
        if (!s || !s->isEnabled()) continue;
        if (s->isBlocking() != blocking) continue;   // dispatch filter

        uint32_t intervalMs = s->getReadIntervalMs();
        if (intervalMs > 0 && (ms - _lastReadMs[i]) < intervalMs) continue;

        // Serialise I2C bus access for non-blocking sensors (#14).
        // Blocking sensors (UART-based: SDS011, PMS5003, Wind) manage their own
        // bus, so only lock for non-blocking I2C reads.
        bool tookMutex = false;
        if (!blocking && wireMutex) {
            tookMutex = (xSemaphoreTake(wireMutex, pdMS_TO_TICKS(100)) == pdTRUE);
        }

        int n = s->readAll(readings, 4);

        if (tookMutex) xSemaphoreGive(wireMutex);
        if (n <= 0) {
            s->incErrorCount();
        }
        if (n > 0) {
            for (int j = 0; j < n; j++) {
                readings[j].timestamp = now;
                strncpy(readings[j].sensorId,   s->getId(),   sizeof(readings[j].sensorId)   - 1);
                strncpy(readings[j].sensorType, s->getType(), sizeof(readings[j].sensorType) - 1);

                if (xQueueSend(queue, &readings[j], 0) != pdTRUE) {
                    extern volatile uint32_t g_queueDrops;
                    g_queueDrops++;
                } else {
                    pushed++;
                }
            }
            _lastReadMs[i] = ms;
            s->setLastReadTs(now);
        }
    }
    return pushed;
}

// ---------------------------------------------------------------------------
int SensorManager::tick(QueueHandle_t sensorQueue, uint32_t now) {
    // Backwards-compat: read all sensors (blocking + non-blocking)
    return tickFiltered(sensorQueue, now, false) +
           tickFiltered(sensorQueue, now, true);
}

// ---------------------------------------------------------------------------
bool SensorManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}

// ---------------------------------------------------------------------------
uint32_t SensorManager::minReadIntervalMs() const {
    uint32_t minMs = 1000;  // default 1s if no sensors
    for (int i = 0; i < _count; i++) {
        if (!_sensors[i] || !_sensors[i]->isEnabled()) continue;
        uint32_t iv = _sensors[i]->getReadIntervalMs();
        if (iv > 0 && iv < minMs) minMs = iv;
    }
    return (minMs < 50) ? 50 : minMs;   // clamp: never poll faster than 50ms
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

// Format a sensor reading for JSON output.
// Drops trailing zeros so "23.50" → "23.5" and integer-like values
// ("400.00" → "400") render cleanly in the UI without per-metric format
// rules.  Buffer is 16 bytes — wider than any plausible sensor reading.
static void _formatValue(float v, char* out, size_t outSz) {
    snprintf(out, outSz, "%.2f", v);
    // Trim trailing zeros after the decimal point, then the point itself.
    char* dot = strchr(out, '.');
    if (!dot) return;
    char* end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *dot = '\0';
}

// ---------------------------------------------------------------------------
void SensorManager::toJson(JsonArray arr) const {
    // Build the per-sensor JSON skeletons first; populate live values in a
    // single critical section after.  Holding the mutex once across the whole
    // sensor list (instead of per-iteration) keeps the ring-buffer producer
    // paused for one short window and removes the partial-result hazard
    // where one sensor gets values and another doesn't because the 20 ms
    // try-take expired mid-loop.
    struct Slot { JsonObject obj; ISensor* sensor; const char* metrics[8]; int mcount; };
    Slot slots[16];
    int  slotCount = 0;

    for (int i = 0; i < _count && slotCount < 16; i++) {
        ISensor* s = _sensors[i];
        if (!s) continue;
        JsonObject o = arr.add<JsonObject>();
        o["id"]           = s->getId();
        o["type"]         = s->getType();
        o["name"]         = s->getName();
        o["enabled"]      = s->isEnabled();
        o["last_read_ts"] = s->lastReadTs();
        o["error_count"]  = s->errorCount();
        // Phase 5c-4 — exposes read_interval_ms so the UI can compute
        // staleness (entry is "stale" once age > 2× this interval).
        o["read_interval_ms"] = s->getReadIntervalMs();
        o["status"]       = s->isEnabled() ? (s->errorCount() > 0 ? "error" : "ok") : "disabled";

        Slot& sl = slots[slotCount++];
        sl.obj    = o;
        sl.sensor = s;
        sl.mcount = s->getMetrics(sl.metrics, 8);

        JsonArray ma = o["metrics"].to<JsonArray>();
        for (int m = 0; m < sl.mcount; m++) ma.add(sl.metrics[m]);
    }

    // Single critical section: scan the ring buffer for every metric of
    // every sensor under one lock acquisition.  ~16 sensors × 8 metrics ×
    // 200-entry strcmp is well under 1 ms on the C3.
    if (xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    for (int i = 0; i < slotCount; i++) {
        Slot& sl = slots[i];
        JsonObject vals = sl.obj["last_values"].to<JsonObject>();
        for (int m = 0; m < sl.mcount; m++) {
            SensorReading r;
            if (!webRingBuf.findLast(sl.sensor->getId(), sl.metrics[m], r)) continue;
            // {v: 23.5, u: "C", ts: 1714492800}
            // Frontend renders the value+unit pair and uses ts for per-metric
            // staleness.  serialized() embeds the raw number string so we
            // don't lose decimal precision through ArduinoJson's float path.
            char vBuf[16];
            _formatValue(r.value, vBuf, sizeof(vBuf));
            JsonObject mv = vals[sl.metrics[m]].to<JsonObject>();
            mv["v"]  = serialized(String(vBuf));
            mv["u"]  = r.unit;
            mv["ts"] = r.timestamp;
        }

        // Per-card sparkline of the *primary* metric — keeps the payload
        // bounded.  32 points covers ~5 min at 10 s read intervals which
        // is enough for a thumbnail trend without inflating /api/sensors
        // beyond a few KB even on devices with 8+ sensors.
        if (sl.mcount > 0) {
            constexpr size_t SPARK_MAX = 32;
            float spark[SPARK_MAX];
            size_t got = webRingBuf.collectMetricSeries(
                sl.sensor->getId(), sl.metrics[0], spark, SPARK_MAX);
            if (got >= 2) {
                JsonArray arr = sl.obj["spark"].to<JsonArray>();
                for (size_t k = 0; k < got; k++) {
                    char b[12];
                    _formatValue(spark[k], b, sizeof(b));
                    arr.add(serialized(String(b)));
                }
            }
        }
    }
    xSemaphoreGive(webDataMutex);
}
