#include "WebhookExporter.h"

bool WebhookExporter::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]     | false;
    if (!_enabled) return true;

    strncpy(_url, cfg["url"] | "", sizeof(_url) - 1);
    _cooldownMs = cfg["cooldown_ms"] | 60000;
    _ruleCount  = 0;

    JsonArrayConst rules = cfg["rules"].as<JsonArrayConst>();
    for (JsonObjectConst r : rules) {
        if (_ruleCount >= MAX_RULES) break;
        Rule& rule = _rules[_ruleCount];
        strncpy(rule.sensorId, r["sensor_id"] | "", sizeof(rule.sensorId) - 1);
        strncpy(rule.metric,   r["metric"]     | "", sizeof(rule.metric)   - 1);
        rule.threshold   = r["threshold"] | 0.0f;
        const char* cond = r["condition"] | "above";
        rule.above       = (strcmp(cond, "above") == 0);
        rule.lastFiredMs = 0;
        if (rule.sensorId[0] && rule.metric[0]) _ruleCount++;
    }

    Serial.printf("[Webhook] url=%s rules=%d cooldown=%ums\n",
                  _url, _ruleCount, _cooldownMs);
    return true;
}

bool WebhookExporter::_fireRule(const Rule& rule, float value, uint32_t ts) {
    HTTPClient http;
    http.begin(_url);
    http.addHeader("Content-Type", "application/json");

    char body[256];
    snprintf(body, sizeof(body),
             "{\"event\":\"threshold_breach\","
             "\"sensor_id\":\"%s\",\"metric\":\"%s\","
             "\"value\":%.4g,\"threshold\":%.4g,"
             "\"condition\":\"%s\",\"ts\":%lu}",
             rule.sensorId, rule.metric,
             value, rule.threshold,
             rule.above ? "above" : "below",
             (unsigned long)ts);

    int code = http.POST(body);
    http.end();

    if (code > 0 && code < 300) {
        Serial.printf("[Webhook] Fired: %s/%s=%.3g threshold=%.3g code=%d\n",
                      rule.sensorId, rule.metric, value, rule.threshold, code);
        return true;
    }
    Serial.printf("[Webhook] Fire failed: code=%d\n", code);
    return false;
}

bool WebhookExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || _ruleCount == 0 || count == 0) return true;
    uint32_t now = millis();

    for (size_t i = 0; i < count; i++) {
        const SensorReading& rd = readings[i];
        for (int j = 0; j < _ruleCount; j++) {
            Rule& rule = _rules[j];
            if (strcmp(rd.sensorId, rule.sensorId) != 0) continue;
            if (strcmp(rd.metric,   rule.metric)   != 0) continue;

            bool breach = rule.above ? (rd.value > rule.threshold)
                                     : (rd.value < rule.threshold);
            if (!breach) continue;

            // Respect cooldown
            if (rule.lastFiredMs != 0 && (now - rule.lastFiredMs) < _cooldownMs) continue;

            if (_fireRule(rule, rd.value, rd.timestamp)) {
                rule.lastFiredMs = now;
            }
        }
    }
    return true;  // webhook failures don't block other exporters
}
