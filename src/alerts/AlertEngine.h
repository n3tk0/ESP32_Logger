#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "../core/SensorTypes.h"

// ============================================================================
// AlertEngine — rule-based alert evaluation for IoT sensor readings.
//
// Rules are persisted in /alerts.json on LittleFS.  evaluate() is called
// from ProcessingTask after every successful sensor read.  When a rule
// condition is met for at least `duration_s` seconds, actions are dispatched:
//   ACTION_TOAST   → pushed to an in-memory toast ring buffer (GET /api/alerts/toasts)
//   ACTION_MQTT    → published to <prefix>/alerts/<rule_id> via MqttExporter
//
// Thread safety: evaluate() is called from the ProcessingTask FreeRTOS task;
// load/save/snooze/toJson/fromJson are called from the AsyncWebServer task.
// A SemaphoreHandle_t guards shared state (rules + history).
// ============================================================================

constexpr int ALERT_MAX_RULES    = 8;   // max simultaneous rules
constexpr int ALERT_HISTORY_MAX  = 16;  // in-RAM history ring
constexpr int ALERT_TOAST_MAX    = 8;   // in-RAM toast ring

// Action bitmask values
constexpr uint8_t ACTION_TOAST   = 0x01;
constexpr uint8_t ACTION_MQTT    = 0x02;

// Epsilon for float equality comparison in _evalOp ("==" operator)
constexpr float ALERT_FLOAT_EPS  = 0.001f;

struct AlertToast {
    char     rule_id[24];
    char     name[48];
    float    value;
    uint32_t ts;
};

class AlertEngine {
public:
    AlertEngine();

    // Called once from setup() after LittleFS is mounted.
    bool begin(fs::FS& fs, const char* path = "/alerts.json");

    // Called from ProcessingTask for every incoming SensorReading.
    // Evaluates all enabled, non-snoozed rules against the reading.
    void evaluate(const SensorReading& r, uint32_t nowTs);

    // Serialise current state (rules + history) for GET /api/alerts.
    void toJson(JsonDocument& doc) const;

    // Replace rules from a POST body (whole-document JSON replace).
    // Saves to LittleFS immediately.  Returns false on parse error.
    bool fromJson(const uint8_t* body, size_t len);

    // Snooze one rule by id until `until_ts` (unix ts; 0 = clear snooze).
    bool snooze(const char* ruleId, uint32_t until_ts);

    // Toast queue — consumed by GET /api/alerts/toasts endpoint.
    bool popToast(AlertToast& out);
    bool hasToasts() const;

    // Returns false if begin() was never called successfully (mutex is null).
    // Callers can check this to surface an error in the UI rather than silently
    // returning empty data.
    bool isHealthy() const { return _mutex != nullptr; }

private:
    // ---- Rule ---------------------------------------------------------------
    struct Rule {
        char     id[24]     = {};
        char     name[48]   = {};
        char     sensor[17] = {};
        char     metric[16] = {};
        char     op[4]      = {};    // ">", "<", ">=", "<=", "=="
        float    threshold  = 0.0f;
        uint32_t duration_s = 0;
        uint8_t  actions    = ACTION_TOAST;
        bool     enabled    = false;
        uint32_t snooze_until = 0;
        // Evaluation state (not persisted)
        uint32_t condFirstMetTs = 0;  // unix ts when condition first became true
        bool     firing         = false;
        uint32_t lastFiredTs    = 0;
    };

    // ---- History entry ------------------------------------------------------
    struct HistEntry {
        uint32_t ts       = 0;
        char     rule_id[24] = {};
        float    value    = 0.0f;
    };

    // ---- State --------------------------------------------------------------
    Rule      _rules[ALERT_MAX_RULES];
    int       _ruleCount = 0;

    HistEntry _history[ALERT_HISTORY_MAX];
    int       _histHead  = 0;   // next write position
    int       _histCount = 0;

    AlertToast _toasts[ALERT_TOAST_MAX];
    int        _toastHead = 0;
    int        _toastTail = 0;

    fs::FS*    _fs       = nullptr;
    char       _path[32] = "/alerts.json";

    SemaphoreHandle_t _mutex = nullptr;

    // R14 / AUDIT 15.8: pending MQTT alerts to drain WITHOUT _mutex held.
    // _dispatch (called under lock) stages the SensorReading here; the
    // evaluate() caller drains the buffer after releasing the mutex so
    // the seconds-long MQTT publish never holds the alert lock.
    // Sized for the worst case where every rule fires in one evaluate
    // call; in practice 1-2 fire per pass.
    static constexpr int PENDING_MQTT_MAX = 8;
    SensorReading _pendingMqtt[PENDING_MQTT_MAX];
    int           _pendingMqttCount = 0;

    // ---- Helpers ------------------------------------------------------------
    bool  _evalOp(float val, const char* op, float threshold) const;
    void  _dispatch(const Rule& rule, float val, uint32_t ts);
    void  _pushToast(const Rule& rule, float val, uint32_t ts);
    void  _appendHistory(const Rule& rule, float val, uint32_t ts);
    bool  _save() const;           // write _rules to _path; caller holds mutex
    bool  _parseRule(JsonObject o, Rule& out) const;
    uint8_t _parseActions(JsonArrayConst arr) const;
};

// Global singleton — defined in AlertEngine.cpp
extern AlertEngine alertEngine;
