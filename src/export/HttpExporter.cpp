#include "HttpExporter.h"
#include <WiFi.h>

bool HttpExporter::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | false;
    if (!_enabled) return true;

    strncpy(_url,    cfg["url"]    | "", sizeof(_url)-1);
    strncpy(_method, cfg["method"] | "POST", sizeof(_method)-1);

    _hdrCount = 0;
    JsonObjectConst headers = cfg["headers"].as<JsonObjectConst>();
    if (!headers.isNull()) {
        for (auto kv : headers) {
            if (_hdrCount >= 4) break;
            strncpy(_hdrKeys[_hdrCount], kv.key().c_str(),    sizeof(_hdrKeys[0])-1);
            strncpy(_hdrVals[_hdrCount], kv.value().as<const char*>() ?: "",
                    sizeof(_hdrVals[0])-1);
            _hdrCount++;
        }
    }

    Serial.printf("[HTTP] url=%s method=%s\n", _url, _method);
    return true;
}

bool HttpExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || count == 0 || _url[0] == '\0') return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    // Build JSON array body
    // Each reading ~80 bytes; allocate dynamically for large batches
    size_t bodyLen = count * 90 + 32;
    char*  body    = new char[bodyLen];
    if (!body) return false;

    size_t pos = 0;
    pos += snprintf(body + pos, bodyLen - pos, "[");
    for (size_t i = 0; i < count; i++) {
        const SensorReading& r = readings[i];
        if (i > 0) pos += snprintf(body + pos, bodyLen - pos, ",");
        pos += snprintf(body + pos, bodyLen - pos,
            "{\"ts\":%lu,\"id\":\"%s\",\"sensor\":\"%s\","
            "\"metric\":\"%s\",\"value\":%.4g,\"unit\":\"%s\",\"q\":%u}",
            (unsigned long)r.timestamp, r.sensorId, r.sensorType,
            r.metric, r.value, r.unit, (unsigned)r.quality);
    }
    pos += snprintf(body + pos, bodyLen - pos, "]");

    HTTPClient http;
    http.begin(_url);
    http.addHeader("Content-Type", "application/json");
    for (int i = 0; i < _hdrCount; i++) {
        http.addHeader(_hdrKeys[i], _hdrVals[i]);
    }

    int code = http.POST(body);
    bool ok  = (code >= 200 && code < 300);
    if (!ok) {
        Serial.printf("[HTTP] POST failed, code=%d\n", code);
    }
    http.end();
    delete[] body;
    return ok;
}
