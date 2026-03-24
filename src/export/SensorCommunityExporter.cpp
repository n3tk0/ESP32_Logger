#include "SensorCommunityExporter.h"
#include "../core/Globals.h"
#include <WiFi.h>

bool SensorCommunityExporter::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]      | false;
    _intervalMs = cfg["interval_ms"]  | 145000;
    strncpy(_deviceId, config.deviceId, sizeof(_deviceId)-1);
    DBGF("[SC] deviceId=%s interval=%lus\n",
                  _deviceId, _intervalMs/1000);
    return true;
}

bool SensorCommunityExporter::_postPin(const char* pin,
                                        const char* sensorName,
                                        const char* body)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    char sensorHeader[32];
    snprintf(sensorHeader, sizeof(sensorHeader), "esp32-%s", _deviceId);

    HTTPClient http;
    http.begin(API_URL);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("X-Pin",         pin);
    http.addHeader("X-Sensor",      sensorHeader);

    int code = http.POST(const_cast<char*>(body));
    bool ok  = (code == 201 || code == 200);
    if (!ok) {
        DBGF("[SC] pin=%s POST failed code=%d\n", pin, code);
    }
    http.end();
    return ok;
}

bool SensorCommunityExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled) return true;
    if (millis() - _lastSendMs < _intervalMs) return true; // rate-limit
    if (WiFi.status() != WL_CONNECTED) return false;

    // Extract values from batch
    float pm25 = NAN, pm10 = NAN;
    float temp = NAN, hum  = NAN, pres = NAN;

    for (size_t i = 0; i < count; i++) {
        const SensorReading& r = readings[i];
        if (strcmp(r.metric, "pm25") == 0) pm25 = r.value;
        if (strcmp(r.metric, "pm10") == 0) pm10 = r.value;
        if (strcmp(r.metric, "temperature") == 0) temp = r.value;
        if (strcmp(r.metric, "humidity")    == 0) hum  = r.value;
        if (strcmp(r.metric, "pressure")    == 0) pres = r.value;
    }

    char body[256];
    bool ok = true;

    // PM sensor (X-Pin 1) — SDS011 / PMS5003
    if (!isnan(pm25) && !isnan(pm10)) {
        snprintf(body, sizeof(body),
            "{\"software_version\":\"WaterLogger v5.0\","
            "\"sensordatavalues\":["
            "{\"value_type\":\"P1\",\"value\":\"%.2f\"},"
            "{\"value_type\":\"P2\",\"value\":\"%.2f\"}"
            "]}",
            pm10, pm25);
        ok &= _postPin("1", "SDS011", body);
    }

    // Environmental (X-Pin 11) — BME280
    if (!isnan(temp) && !isnan(hum) && !isnan(pres)) {
        snprintf(body, sizeof(body),
            "{\"software_version\":\"WaterLogger v5.0\","
            "\"sensordatavalues\":["
            "{\"value_type\":\"temperature\",\"value\":\"%.2f\"},"
            "{\"value_type\":\"humidity\",\"value\":\"%.2f\"},"
            "{\"value_type\":\"pressure\",\"value\":\"%.2f\"}"
            "]}",
            temp, hum, pres * 100.0f); // hPa → Pa for API
        ok &= _postPin("11", "BME280", body);
    }

    if (ok) _lastSendMs = millis();
    return ok;
}
