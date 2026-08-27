#include "SensorManager.h"
#include "../utils/MutexGuard.h"
#include <LittleFS.h>
#include "../pipeline/DataPipeline.h"  // wireMutex (#14)
#include "../core/BoardProfiles.h"     // g_pinAllowUnsafe (per-sensor pin override)
#include "ReadingCache.h"              // latest-value table (cross-sensor lookups)
#include "I2CBus.h"                    // per-bus arbitration + reset on reload

SensorManager sensorManager;

// ---------------------------------------------------------------------------
// Serial1 ownership arbitration (row 21.1).
// Only one sensor plugin may initialise Serial1; the second call from a
// different plugin returns false so init() can refuse to proceed.
static ISensor* _serial1Owner = nullptr;

bool _claimSerial1(ISensor* who) {
    if (_serial1Owner == nullptr) { _serial1Owner = who; return true; }
    return (_serial1Owner == who);
}
void _releaseSerial1(ISensor* who) {
    if (_serial1Owner == who) _serial1Owner = nullptr;
}

// ---------------------------------------------------------------------------
// I2C address ownership arbitration (row 22.4).
// Tracks which plugin claimed each 7-bit I2C address so that two sensors
// with the same fixed address refuse to co-initialise rather than silently
// corrupting each other's reads.
//
// Scoped PER BUS: an address only collides with another device on the same
// wire. This is what lets VEML6075 and VEML7700 — both hard-wired to 0x10 with
// no address-select pin — run at the same time, one on each controller.
static ISensor* _i2cOwners[I2CBus::MAX_BUSES][128] = {};

bool _claimI2cAddress(uint8_t bus, uint8_t addr, ISensor* who) {
    if (bus >= I2CBus::MAX_BUSES || addr >= 128) return false;
    if (_i2cOwners[bus][addr] == nullptr) { _i2cOwners[bus][addr] = who; return true; }
    return (_i2cOwners[bus][addr] == who);
}
void _releaseI2cClaims(ISensor* who) {
    if (!who) return;
    for (int b = 0; b < I2CBus::MAX_BUSES; b++) {
        for (int i = 0; i < 128; i++) {
            if (_i2cOwners[b][i] == who) _i2cOwners[b][i] = nullptr;
        }
    }
}

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
    memset(_health,     0, sizeof(_health));
    // Reset arbitration tables so a reload or a failed-init doesn't
    // permanently block re-claiming the same resource.
    _serial1Owner = nullptr;
    memset(_i2cOwners, 0, sizeof(_i2cOwners));
    // Release the I2C controllers as well: the recorded pin assignment would
    // otherwise outlive the sensors that asked for it and refuse the new
    // config's pins as a conflict.
    //
    // Under wireMutex, because resetAll() calls TwoWire::end(), which deinits
    // the peripheral AND frees the driver's rx/tx buffers. /api/i2c_scan walks
    // 127 addresses holding only wireMutex, so without this the buffers could
    // be freed mid-scan.
    //
    // Lock order is configMutex → wireMutex, matching tickFiltered (callers of
    // _destroyAll already hold configMutex). i2c_scan takes wireMutex alone, so
    // no cycle exists.
    {
        MutexGuard wg(wireMutex, pdMS_TO_TICKS(1000));
        if (wireMutex && !wg.isLocked()) {
            // A scan holds the bus for ~130 ms at worst, so a 1 s timeout means
            // something is genuinely wrong. Resetting anyway is the lesser evil:
            // leaving the registry claiming buses the new config never brought
            // up would refuse every subsequent pin assignment.
            Serial.println("[SensorManager] _destroyAll: wireMutex timeout — resetting I2C anyway");
        }
        I2CBus::resetAll();
    }
    // Drop cached values too: entries for sensors that no longer exist would
    // otherwise keep answering get() with an ever-growing age instead of
    // "unknown", which reads the same as a live-but-stale sensor.
    readingCache.clear();
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
        // Per-sensor pin override: while set, validateAttachPin() downgrades a
        // strapping/reserved-pin refusal to a warning (flash/out-of-range stay
        // hard-blocked). Cleared right after init so it can't leak to anything
        // else that validates pins.
        g_pinAllowUnsafe = sensor["allow_unsafe_pins"] | false;
        bool initOk = s->init(sensor);
        g_pinAllowUnsafe = false;
        if (initOk) {
            _sensors[_count]    = s;
            _lastReadMs[_count] = 0;
            _count++;
            initialised++;
            Serial.printf("[SensorManager] Sensor '%s' (%s) ready\n", id, type);
        } else {
            Serial.printf("[SensorManager] Sensor '%s' init FAILED\n", id);
            // R17 follow-up (Codex P2 on PR #93): release any claims this
            // plugin made on Serial1 / I2C addresses before init failed —
            // otherwise the claim outlives the (deleted) instance and
            // permanently blocks subsequent plugins on the same resource.
            _releaseSerial1(s);
            _releaseI2cClaims(s);
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

    // Up to 8 metrics per sensor per tick.  The fattest producer is BME68x at
    // 7 (T/H/P/gas/IAQ + dew_point + humidity_amb); SPS30 emits 5
    // (4 × PM + device_status).  Keep this >= the largest getMetrics() count
    // of any registered plugin — readAll() silently truncates otherwise.
    SensorReading readings[8];
    constexpr int MAX_METRICS_PER_TICK = 8;

    // R14 / AUDIT 3.19 + 15.3: hold configMutex for the read iteration so
    // a concurrent reloadConfig() can't _destroyAll() the sensor pointer
    // array we're iterating. Reload acquires the same mutex with a longer
    // timeout (see SensorManager::reloadConfig). Bounded 1 s take here —
    // if reload is mid-flight we skip this tick rather than spin.
    MutexGuard sg(configMutex, pdMS_TO_TICKS(1000));
    if (configMutex && !sg.isLocked()) {
        return 0;   // reload in flight; skip silently, retry next tick
    }

    for (int i = 0; i < _count; i++) {
        ISensor* s = _sensors[i];
        if (!s || !s->isEnabled()) continue;
        if (s->isBlocking() != blocking) continue;   // dispatch filter

        uint32_t intervalMs = s->getReadIntervalMs();
        if (intervalMs > 0 && (ms - _lastReadMs[i]) < intervalMs) continue;

        // Serialise I2C bus access for non-blocking sensors (#14).
        // Blocking sensors (UART-based: SDS011, PMS5003, Wind) manage their own
        // bus, so only lock for non-blocking I2C reads.
        int n = 0;
        uint32_t t0us = 0, latUs = 0;
        if (!blocking && wireMutex) {
            MutexGuard wg(wireMutex, pdMS_TO_TICKS(100));
            if (!wg.isLocked()) {
                Serial.println("[SensorManager] wireMutex busy — skipping sensor read");
                continue;
            }
            t0us = micros();
            n = s->readAll(readings, MAX_METRICS_PER_TICK);
            latUs = (uint32_t)(micros() - t0us);
            // wg releases wireMutex here, before health tracking
        } else {
            t0us = micros();
            n = s->readAll(readings, MAX_METRICS_PER_TICK);
            latUs = (uint32_t)(micros() - t0us);
        }

        // ------------------------------------------------------------------
        // Health tracking — rotate hourly buckets for every elapsed hour.
        // Loop (not if) so multi-hour gaps mark intermediate slots as
        // "unknown" rather than leaving stale data in them.
        // ------------------------------------------------------------------
        {
            constexpr uint32_t ONE_HOUR_MS = 3600UL * 1000UL;
            HealthData& h = _health[i];
            if (h.slotStartMs == 0) h.slotStartMs = ms;   // first ever read
            if (h.firstSeenMs == 0) h.firstSeenMs = ms;   // stable init reference
            while ((ms - h.slotStartMs) >= ONE_HOUR_MS) {
                // Advance to next slot, clear its accumulators.
                // Use += ONE_HOUR_MS (not =ms) so overruns don't accumulate drift.
                h.curSlot = (h.curSlot + 1) % 24;
                h.hourReads [h.curSlot] = 0;
                h.hourErrors[h.curSlot] = 0;
                h.hourLatUs [h.curSlot] = 0;
                h.slotStartMs += ONE_HOUR_MS;
            }
            // A periodic ("duty-cycled") sensor is asleep between wake cycles,
            // so most polls legitimately return nothing. Treat an empty poll as
            // an expected sleep — neither a read nor an error — UNLESS the
            // sensor is overdue (silent for >2 expected data intervals since its
            // last success), which signals a real fault. _lastReadMs[i] is the
            // last SUCCESSFUL read (updated below, after this block).
            bool expectedSleep = false;
            if (n <= 0 && !s->countEmptyReadAsError()) {
                // Reference = last successful read, or the sensor's first-seen
                // time before it has ever read. NOT slotStartMs: that rotates
                // hourly, so for a work period >= 30 min (dataIntervalMs*2 >= 1h)
                // it would keep the sensor "expected-sleeping" forever and never
                // flag a dead/failed sensor. firstSeenMs is set once and zeroed
                // only on reload (_destroyAll), so it stays a stable per-instance
                // reference.
                uint32_t ref  = (_lastReadMs[i] != 0) ? _lastReadMs[i] : h.firstSeenMs;
                expectedSleep = (ms - ref) <= (s->dataIntervalMs() * 2);
            }

            if (n > 0) {
                h.hourReads [h.curSlot]++;
                h.hourLatUs [h.curSlot] += latUs;
                h.totalLatUs += latUs;
                h.latSamples++;
                // Recovered: clear a prior 'overdue' fault so status returns OK.
                if (s->isPeriodic()) s->resetErrorCount();
            } else if (!expectedSleep) {
                // Rate-limit overdue errors for a periodic sensor to one per
                // expected data interval, so a dead sensor records ~1 missed
                // reading per period instead of one per 1 s poll — otherwise a
                // 10-min outage would log ~600 errors and crush uptime%.
                // Continuous sensors (countEmptyReadAsError) count every miss.
                if (s->countEmptyReadAsError() || (ms - h.lastErrorMs) >= s->dataIntervalMs()) {
                    h.hourErrors[h.curSlot]++;
                    s->incErrorCount();
                    h.lastErrorMs = ms;
                }
            }
            // expectedSleep: leave every counter untouched (neutral poll).
        }
        if (n > 0) {
            for (int j = 0; j < n; j++) {
                // Stamped on arrival ONLY when the plugin did not know.
                //
                // Almost none do: a wired sensor is read now, so `now` is the
                // truth and every plugin leaves this zero. The exception is a
                // remote node handing over readings it buffered through an
                // outage — those were taken minutes or hours ago, and stamping
                // them with `now` would collapse the whole outage onto one
                // instant and file it under the wrong hour.
                //
                // This used to overwrite unconditionally, which made the node's
                // buffering pointless: the readings arrived and were all dated
                // the moment they arrived.
                if (readings[j].timestamp == 0) readings[j].timestamp = now;
                strncpy(readings[j].sensorId,   s->getId(),   sizeof(readings[j].sensorId)   - 1);
                strncpy(readings[j].sensorType, s->getType(), sizeof(readings[j].sensorType) - 1);

                // Publish to the latest-value table BEFORE queueing. Consumers
                // that need cross-sensor values (BME688's ambient-RH reference,
                // HeaterModule's control inputs) read it here rather than off
                // the queue, so a backed-up sensorQueue can't starve them.
                readingCache.put(readings[j]);

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
    // R14 / AUDIT 3.19 + 15.3: writer-side companion of tickFiltered's
    // 1 s read-side acquire. Longer timeout (8 s) covers SDS011/PMS5003
    // blocking reads (~2 s each), I2C bus serialisation, and the worst-
    // case SCD4x init delay (5.1 s). If we can't acquire, the existing
    // sensor table stays valid; caller surfaces the failure to the user.
    MutexGuard sg(configMutex, pdMS_TO_TICKS(8000));
    if (configMutex && !sg.isLocked()) {
        Serial.println("[SensorManager] reloadConfig: configMutex timeout — aborted");
        return false;
    }
    return loadAndInit(fs, cfgPath);
}

// ---------------------------------------------------------------------------
uint32_t SensorManager::minReadIntervalMs() const {
    // R28 follow-up (PR #106 Codex P1 + Gemini High): hold configMutex while
    // iterating _sensors[]. SensorTask now re-reads this every loop iteration
    // (audit row 10.1); without the lock a concurrent reloadConfig() can
    // _destroyAll() the array mid-walk → UAF. Bounded 200 ms take — if reload
    // is mid-flight return a safe default so SensorTask keeps ticking at 1 Hz
    // until the next iteration.
    MutexGuard sg(configMutex, pdMS_TO_TICKS(200));
    if (configMutex && !sg.isLocked()) {
        return 1000;   // reload in flight; safe fallback cadence
    }

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
    struct Slot { JsonObject obj; ISensor* sensor; const char* metrics[8]; int mcount; int idx; };
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
        // data_interval_ms is the expected spacing between actual readings (the
        // work period for a duty-cycled sensor; == read interval otherwise). The
        // UI bases its freshness window on this, and shows a "sleeping" state for
        // periodic sensors so an intentional sleep isn't read as "stale".
        o["data_interval_ms"] = s->dataIntervalMs();
        o["periodic"]         = s->isPeriodic();
        o["status"]       = s->isEnabled() ? (s->errorCount() > 0 ? "error" : "ok") : "disabled";

        Slot& sl = slots[slotCount++];
        sl.obj    = o;
        sl.sensor = s;
        sl.idx    = i;
        sl.mcount = s->getMetrics(sl.metrics, 8);

        JsonArray ma = o["metrics"].to<JsonArray>();
        for (int m = 0; m < sl.mcount; m++) ma.add(sl.metrics[m]);
    }

    // Single critical section: scan the ring buffer for every metric of
    // every sensor under one lock acquisition.  Cost is bounded by
    // RING_SCAN_LIMIT_LAST / RING_SCAN_LIMIT_SERIES rather than by ring
    // capacity — without those the same loop would walk a 58 000-entry PSRAM
    // ring once per missing metric, while ProcessingTask waits 5 ms for this
    // very mutex before dropping a reading.
    // Guard against legacy / early-boot path where mutexes are still nullptr.
    if (!webDataMutex || xSemaphoreTake(webDataMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
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

    // ------------------------------------------------------------------
    // Health objects — appended outside the mutex (health arrays are
    // written only from SensorTask; a torn read here is a best-effort
    // display glitch, not a data-corruption risk).
    // ------------------------------------------------------------------
    uint32_t nowMs = millis();
    for (int i = 0; i < slotCount; i++) {
        const HealthData& h  = _health[slots[i].idx];
        uint32_t  lastMs     = _lastReadMs[slots[i].idx];

        uint32_t reads = 0, errors = 0, latSum = 0;
        for (int b = 0; b < 24; b++) {
            reads  += h.hourReads[b];
            errors += h.hourErrors[b];
            latSum += h.hourLatUs[b];
        }
        uint32_t total  = reads + errors;
        // Latency is accumulated only on successful reads, so divide by reads
        // only — avoids systematic underreporting as error counts grow.
        uint32_t avgLat = (reads > 0) ? (latSum / reads) : 0;
        float uptime = (total > 0) ? (100.0f * (float)reads / (float)total) : 100.0f;

        JsonObject ho = slots[i].obj["health"].to<JsonObject>();
        ho["reads_24h"]        = reads;
        ho["errors_24h"]       = errors;
        ho["avg_latency_us"]   = avgLat;
        ho["last_read_ms_ago"] = (lastMs > 0) ? (nowMs - lastMs) : 0;
        // Direct float assignment — avoids the temporary String object and
        // heap fragmentation caused by serialized(String(uptime, 1)).
        ho["uptime_pct_24h"]   = (float)((int)(uptime * 10 + 0.5f)) / 10.0f;

        // Buckets oldest → newest (curSlot+1 is the oldest slot)
        JsonArray barr = ho["uptime_buckets_24h"].to<JsonArray>();
        uint8_t slot = (h.curSlot + 1) % 24;
        for (int b = 0; b < 24; b++) {
            uint32_t r = h.hourReads[slot];
            uint32_t e = h.hourErrors[slot];
            if (r == 0 && e == 0)      barr.add("unknown");
            else if (e == 0)           barr.add("ok");
            else if (e * 4 < r)        barr.add("warn");   // <25% error rate
            else                       barr.add("err");
            slot = (slot + 1) % 24;
        }
    }
}
