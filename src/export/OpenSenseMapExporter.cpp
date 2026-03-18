#include "OpenSenseMapExporter.h"

bool OpenSenseMapExporter::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | false;
    if (!_enabled) return true;

    strncpy(_boxId, cfg["box_id"]       | "", sizeof(_boxId)-1);
    strncpy(_token, cfg["access_token"] | "", sizeof(_token)-1);

    _sensorIdCount = 0;
    JsonObjectConst ids = cfg["sensor_ids"].as<JsonObjectConst>();
    if (!ids.isNull()) {
        for (auto kv : ids) {
            if (_sensorIdCount >= 12) break;
            strncpy(_sensorIds[_sensorIdCount].metric,
                    kv.key().c_str(), sizeof(_sensorIds[0].metric)-1);
            strncpy(_sensorIds[_sensorIdCount].sensorId,
                    kv.value().as<const char*>() ?: "",
                    sizeof(_sensorIds[0].sensorId)-1);
            _sensorIdCount++;
        }
    }

    Serial.printf("[OSM] boxId=%s sensors=%d\n", _boxId, _sensorIdCount);
    return true;
}

const char* OpenSenseMapExporter::_lookupSensorId(const char* metric) const {
    for (int i = 0; i < _sensorIdCount; i++) {
        if (strcmp(_sensorIds[i].metric, metric) == 0) {
            return _sensorIds[i].sensorId;
        }
    }
    return nullptr;
}

bool OpenSenseMapExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || _boxId[0] == '\0' || count == 0) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    // Build JSON array: [{sensorId, value, createdAt}, ...]
    // Only include readings that have a mapped sensorId
    size_t bodyLen = count * 80 + 32;
    char*  body    = new char[bodyLen];
    if (!body) return false;

    size_t pos    = 0;
    int    mapped = 0;
    pos += snprintf(body + pos, bodyLen - pos, "[");

    for (size_t i = 0; i < count; i++) {
        const char* sid = _lookupSensorId(readings[i].metric);
        if (!sid || sid[0] == '\0') continue;

        if (mapped > 0) pos += snprintf(body + pos, bodyLen - pos, ",");
        pos += snprintf(body + pos, bodyLen - pos,
            "{\"sensor\":\"%s\",\"value\":\"%.4g\"}",
            sid, readings[i].value);
        mapped++;
    }
    pos += snprintf(body + pos, bodyLen - pos, "]");

    bool ok = true;
    if (mapped > 0) {
        char url[128];
        snprintf(url, sizeof(url), "%s%s/data", API_BASE, _boxId);

        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type",  "application/json");
        http.addHeader("Authorization", (String("Bearer ") + _token).c_str());

        int code = http.POST(body);
        ok = (code >= 200 && code < 300);
        if (!ok) Serial.printf("[OSM] POST failed code=%d\n", code);
        http.end();
    }

    delete[] body;
    return ok;
}
