#pragma once
#include "IExporter.h"
#include <HTTPClient.h>

// ============================================================================
// WebhookExporter — fires an HTTP POST when a sensor metric crosses a threshold.
//
// Config (platform_config.json → export → webhook):
//   "enabled": true,
//   "url": "https://example.com/webhook",
//   "cooldown_ms": 60000,    // minimum ms between repeated alerts (default 60s)
//   "rules": [
//     { "sensor_id": "temp_indoor", "metric": "temperature",
//       "threshold": 30.0, "condition": "above" },
//     { "sensor_id": "ac_current",  "metric": "current_arms",
//       "threshold": 10.0, "condition": "above" }
//   ]
//
// Conditions: "above" | "below"
//
// Payload on trigger (JSON POST):
//   { "event":"threshold_breach", "sensor_id":"...", "metric":"...",
//     "value":..., "threshold":..., "condition":"above|below",
//     "ts":... }
// ============================================================================
class WebhookExporter : public IExporter {
public:
    static constexpr int MAX_RULES = 8;

    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "webhook"; }
    bool        isEnabled() const override { return _enabled; }

private:
    struct Rule {
        char     sensorId[17];
        char     metric[33];
        float    threshold;
        bool     above;         // true = "above", false = "below"
        uint32_t lastFiredMs;   // millis() of last trigger (for cooldown)
    };

    bool _fireRule(const Rule& rule, float value, uint32_t ts);

    char     _url[129]      = {};
    uint32_t _cooldownMs    = 60000;
    Rule     _rules[MAX_RULES];
    int      _ruleCount     = 0;
};
