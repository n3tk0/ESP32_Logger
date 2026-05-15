#include "AlertEngine.h"
#include "../utils/MutexGuard.h"
#include <LittleFS.h>
#include "../export/MqttExporter.h"

// Forward-declared in Logger.ino (same sketch scope as ApiHandlers.cpp)
#ifdef EXPORT_MQTT_ENABLED
extern MqttExporter* g_mqttExporter;
#endif

AlertEngine alertEngine;

// ---------------------------------------------------------------------------
// NOTE: global C++ constructors run via __libc_init_array() BEFORE
// app_main() / the FreeRTOS scheduler starts on ESP32-C3.  Any
// xSemaphoreCreate*() call at that point returns NULL because the heap
// allocator is not yet ready.  We therefore defer mutex creation to
// begin(), which is always called from application code after the
// scheduler is running.
AlertEngine::AlertEngine() {
    _mutex = nullptr;   // created in begin()
}

// ---------------------------------------------------------------------------
bool AlertEngine::begin(fs::FS& fs, const char* path) {
    // Create the mutex on first call (safe: scheduler is running by now).
    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
        if (!_mutex) {
            // CRITICAL: without a mutex every AlertEngine operation is a no-op.
            // This is only possible if the FreeRTOS heap is exhausted — extremely
            // unlikely in normal operation, but the condition is visible via
            // GET /api/alerts → { "error": "mutex_init_failed" } and here.
            Serial.println("[AlertEngine] CRITICAL: mutex create FAILED — alert system disabled");
            return false;
        }
    }

    _fs = &fs;
    strncpy(_path, path, sizeof(_path) - 1);
    _path[sizeof(_path) - 1] = '\0';

    File f = fs.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[AlertEngine] %s not found — starting empty\n", path);
        return true;   // not an error; first run
    }

    if (f.size() > 8 * 1024) {
        Serial.printf("[AlertEngine] %s too large\n", path);
        f.close();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[AlertEngine] JSON parse error: %s\n", err.c_str());
        return false;
    }

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    // Load rules
    _ruleCount = 0;
    JsonArray rules = doc["rules"].as<JsonArray>();
    for (JsonObject o : rules) {
        if (_ruleCount >= ALERT_MAX_RULES) break;
        if (_parseRule(o, _rules[_ruleCount])) {
            _ruleCount++;
        }
    }

    // Restore persisted history ring so alert history survives reboots
    _histHead  = 0;
    _histCount = 0;
    JsonArray hist = doc["history"].as<JsonArray>();
    for (JsonObject ho : hist) {
        if (_histCount >= ALERT_HISTORY_MAX) break;
        HistEntry& e = _history[_histHead];
        e.ts    = ho["ts"]    | (uint32_t)0;
        e.value = ho["value"] | 0.0f;
        strncpy(e.rule_id, ho["rule_id"] | "", sizeof(e.rule_id) - 1);
        e.rule_id[sizeof(e.rule_id) - 1] = '\0';
        _histHead = (_histHead + 1) % ALERT_HISTORY_MAX;
        _histCount++;
    }

    xSemaphoreGive(_mutex);
    Serial.printf("[AlertEngine] loaded %d rule(s), %d history entries from %s\n",
                  _ruleCount, _histCount, path);
    return true;
}

// ---------------------------------------------------------------------------
bool AlertEngine::_parseRule(JsonObject o, Rule& r) const {
    const char* id = o["id"] | "";
    if (!id[0]) return false;

    strncpy(r.id,     id,               sizeof(r.id)     - 1); r.id    [sizeof(r.id)    - 1] = '\0';
    strncpy(r.name,   o["name"] | id,   sizeof(r.name)   - 1); r.name  [sizeof(r.name)  - 1] = '\0';
    r.enabled      = o["enabled"] | false;
    r.snooze_until = o["snooze_until"] | (uint32_t)0;

    JsonObjectConst expr = o["expr"];
    strncpy(r.sensor, expr["sensor"] | "",  sizeof(r.sensor) - 1); r.sensor[sizeof(r.sensor) - 1] = '\0';
    strncpy(r.metric, expr["metric"] | "",  sizeof(r.metric) - 1); r.metric[sizeof(r.metric) - 1] = '\0';
    strncpy(r.op,     expr["op"]     | ">", sizeof(r.op)     - 1); r.op    [sizeof(r.op)     - 1] = '\0';
    r.threshold  = expr["value"]      | 0.0f;
    r.duration_s = expr["duration_s"] | (uint32_t)0;

    // Reset runtime state when loading
    r.condFirstMetTs = 0;
    r.firing         = false;
    r.lastFiredTs    = 0;

    r.actions = _parseActions(o["actions"].as<JsonArrayConst>());
    if (r.actions == 0) r.actions = ACTION_TOAST;  // default

    return true;
}

// ---------------------------------------------------------------------------
uint8_t AlertEngine::_parseActions(JsonArrayConst arr) const {
    uint8_t mask = 0;
    for (JsonVariantConst v : arr) {
        const char* s = v.as<const char*>();
        if (!s) continue;
        if (strcmp(s, "toast")   == 0) mask |= ACTION_TOAST;
        if (strcmp(s, "mqtt")    == 0) mask |= ACTION_MQTT;
    }
    return mask;
}

// ---------------------------------------------------------------------------
void AlertEngine::evaluate(const SensorReading& r, uint32_t nowTs) {
    if (!_mutex) return;
    MutexGuard g(_mutex, pdMS_TO_TICKS(5));
    if (!g.isLocked()) return;

    for (int i = 0; i < _ruleCount; i++) {
        Rule& rule = _rules[i];
        if (!rule.enabled) continue;
        if (rule.snooze_until && nowTs < rule.snooze_until) continue;

        // Does this reading match the rule's sensor + metric?
        if (strcmp(r.sensorId, rule.sensor) != 0) continue;
        if (strcmp(r.metric,   rule.metric) != 0) continue;

        bool cond = _evalOp(r.value, rule.op, rule.threshold);

        if (cond) {
            if (rule.condFirstMetTs == 0) rule.condFirstMetTs = nowTs;

            bool durationMet = (rule.duration_s == 0) ||
                               ((nowTs - rule.condFirstMetTs) >= rule.duration_s);

            if (!rule.firing && durationMet) {
                rule.firing      = true;
                rule.lastFiredTs = nowTs;
                _dispatch(rule, r.value, nowTs);
            }
        } else {
            // Condition cleared — reset state
            rule.condFirstMetTs = 0;
            rule.firing         = false;
        }
    }
}

// ---------------------------------------------------------------------------
bool AlertEngine::_evalOp(float val, const char* op, float threshold) const {
    if (strcmp(op, ">")  == 0) return val >  threshold;
    if (strcmp(op, "<")  == 0) return val <  threshold;
    if (strcmp(op, ">=") == 0) return val >= threshold;
    if (strcmp(op, "<=") == 0) return val <= threshold;
    if (strcmp(op, "==") == 0) return fabsf(val - threshold) < ALERT_FLOAT_EPS;
    return false;
}

// ---------------------------------------------------------------------------
void AlertEngine::_dispatch(const Rule& rule, float val, uint32_t ts) {
    // NOTE: called with _mutex held — keep quick, no blocking I/O
    _appendHistory(rule, val, ts);

    if (rule.actions & ACTION_TOAST) {
        _pushToast(rule, val, ts);
    }

#ifdef EXPORT_MQTT_ENABLED
    if ((rule.actions & ACTION_MQTT) && g_mqttExporter) {
        // Build a compact JSON payload and publish.
        // We cannot call MqttExporter::send() here (it's not thread-safe for
        // arbitrary strings), so we publish directly via the underlying
        // MQTT_Mini client through a synthesised SensorReading that the
        // exporter's normal send() path can handle.
        // Build topic: <prefix>/alerts/<rule_id>
        char topic[80];
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"rule_id\":\"%s\",\"value\":%.4g,\"ts\":%lu}",
                 rule.id, (double)val, (unsigned long)ts);
        // We don't have direct access to _topicPrefix here, so we publish a
        // synthetic SensorReading via the exporter's public send() interface.
        // Encode the alert as metric="alert" so the exporter routes it to
        //   <prefix>/device/<dev>/sensor/<rule_id>/alert
        SensorReading ar;
        strncpy(ar.sensorId,   rule.id,   sizeof(ar.sensorId)   - 1);
        strncpy(ar.sensorType, "alert",   sizeof(ar.sensorType) - 1);
        strncpy(ar.metric,     "fired",   sizeof(ar.metric)     - 1);
        strncpy(ar.unit,       "",        sizeof(ar.unit)       - 1);
        ar.value     = val;
        ar.timestamp = ts;
        ar.quality   = QUALITY_GOOD;
        g_mqttExporter->send(&ar, 1);
    }
#endif
}

// ---------------------------------------------------------------------------
void AlertEngine::_pushToast(const Rule& rule, float val, uint32_t ts) {
    AlertToast& t = _toasts[_toastHead];
    strncpy(t.rule_id, rule.id,   sizeof(t.rule_id) - 1);
    strncpy(t.name,    rule.name, sizeof(t.name)    - 1);
    t.value = val;
    t.ts    = ts;
    _toastHead = (_toastHead + 1) % ALERT_TOAST_MAX;
    // If ring is full, tail advances too (oldest toast dropped)
    if (_toastHead == _toastTail) {
        _toastTail = (_toastTail + 1) % ALERT_TOAST_MAX;
    }
}

// ---------------------------------------------------------------------------
void AlertEngine::_appendHistory(const Rule& rule, float val, uint32_t ts) {
    HistEntry& e = _history[_histHead];
    e.ts    = ts;
    e.value = val;
    strncpy(e.rule_id, rule.id, sizeof(e.rule_id) - 1);
    _histHead = (_histHead + 1) % ALERT_HISTORY_MAX;
    if (_histCount < ALERT_HISTORY_MAX) _histCount++;
}

// ---------------------------------------------------------------------------
bool AlertEngine::popToast(AlertToast& out) {
    if (!_mutex) return false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool got = (_toastHead != _toastTail);
    if (got) {
        out = _toasts[_toastTail];
        _toastTail = (_toastTail + 1) % ALERT_TOAST_MAX;
    }
    xSemaphoreGive(_mutex);
    return got;
}

// ---------------------------------------------------------------------------
bool AlertEngine::hasToasts() const {
    return _toastHead != _toastTail;
}

// ---------------------------------------------------------------------------
void AlertEngine::toJson(JsonDocument& doc) const {
    if (!_mutex) {
        // Surface the failure so the UI can show a meaningful error instead of
        // silently rendering an empty alerts panel.
        doc["ok"]    = false;
        doc["error"] = "mutex_init_failed";
        doc["rules"].to<JsonArray>();
        doc["history"].to<JsonArray>();
        return;
    }
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    JsonArray rArr = doc["rules"].to<JsonArray>();
    for (int i = 0; i < _ruleCount; i++) {
        const Rule& rule = _rules[i];
        JsonObject o = rArr.add<JsonObject>();
        o["id"]          = rule.id;
        o["name"]        = rule.name;
        o["enabled"]     = rule.enabled;
        o["snooze_until"] = rule.snooze_until;
        o["firing"]      = rule.firing;
        o["last_fired"]  = rule.lastFiredTs;

        JsonObject expr = o["expr"].to<JsonObject>();
        expr["sensor"]     = rule.sensor;
        expr["metric"]     = rule.metric;
        expr["op"]         = rule.op;
        expr["value"]      = rule.threshold;
        expr["duration_s"] = rule.duration_s;

        JsonArray acts = o["actions"].to<JsonArray>();
        if (rule.actions & ACTION_TOAST) acts.add("toast");
        if (rule.actions & ACTION_MQTT)  acts.add("mqtt");
    }

    // History — oldest-first
    JsonArray hArr = doc["history"].to<JsonArray>();
    int start = (_histCount < ALERT_HISTORY_MAX)
                ? 0
                : _histHead;   // oldest slot when ring is full
    for (int n = 0; n < _histCount; n++) {
        int idx = (start + n) % ALERT_HISTORY_MAX;
        const HistEntry& e = _history[idx];
        JsonObject ho = hArr.add<JsonObject>();
        ho["ts"]      = e.ts;
        ho["rule_id"] = e.rule_id;
        ho["value"]   = e.value;
        ho["outcome"] = "fired";
    }

    xSemaphoreGive(_mutex);
}

// ---------------------------------------------------------------------------
bool AlertEngine::fromJson(const uint8_t* body, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc,
                                               (const char*)body, len);
    if (err) return false;

    if (!_mutex) return false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    _ruleCount = 0;
    JsonArray rules = doc["rules"].as<JsonArray>();
    for (JsonObject o : rules) {
        if (_ruleCount >= ALERT_MAX_RULES) break;
        if (_parseRule(o, _rules[_ruleCount])) {
            _ruleCount++;
        }
    }

    bool ok = _save();
    xSemaphoreGive(_mutex);
    return ok;
}

// ---------------------------------------------------------------------------
bool AlertEngine::snooze(const char* ruleId, uint32_t until_ts) {
    if (!_mutex) return false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    bool found = false;
    for (int i = 0; i < _ruleCount; i++) {
        if (strcmp(_rules[i].id, ruleId) == 0) {
            _rules[i].snooze_until  = until_ts;
            _rules[i].firing        = false;
            _rules[i].condFirstMetTs = 0;
            found = true;
            break;
        }
    }

    if (found) _save();
    xSemaphoreGive(_mutex);
    return found;
}

// ---------------------------------------------------------------------------
// Caller must hold _mutex.
bool AlertEngine::_save() const {
    if (!_fs) return false;

    JsonDocument doc;
    JsonArray rArr = doc["rules"].to<JsonArray>();
    for (int i = 0; i < _ruleCount; i++) {
        const Rule& rule = _rules[i];
        JsonObject o = rArr.add<JsonObject>();
        o["id"]           = rule.id;
        o["name"]         = rule.name;
        o["enabled"]      = rule.enabled;
        o["snooze_until"] = rule.snooze_until;

        JsonObject expr = o["expr"].to<JsonObject>();
        expr["sensor"]     = rule.sensor;
        expr["metric"]     = rule.metric;
        expr["op"]         = rule.op;
        expr["value"]      = rule.threshold;
        expr["duration_s"] = rule.duration_s;

        JsonArray acts = o["actions"].to<JsonArray>();
        if (rule.actions & ACTION_TOAST) acts.add("toast");
        if (rule.actions & ACTION_MQTT)  acts.add("mqtt");
    }
    // Persist history ring too so it survives reboots
    JsonArray hArr = doc["history"].to<JsonArray>();
    int start = (_histCount < ALERT_HISTORY_MAX) ? 0 : _histHead;
    for (int n = 0; n < _histCount; n++) {
        int idx = (start + n) % ALERT_HISTORY_MAX;
        const HistEntry& e = _history[idx];
        JsonObject ho = hArr.add<JsonObject>();
        ho["ts"]      = e.ts;
        ho["rule_id"] = e.rule_id;
        ho["value"]   = e.value;
        ho["outcome"] = "fired";
    }

    File f = _fs->open(_path, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}
