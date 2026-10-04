#include "OpenSenseMapExporter.h"
#include <new>            // std::nothrow
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>          // gmtime_r / strftime for createdAt

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
            _sensorIds[_sensorIdCount].metric[sizeof(_sensorIds[0].metric)-1] = '\0';
            
            strncpy(_sensorIds[_sensorIdCount].sensorId,
                    kv.value().as<const char*>() ?: "",
                    sizeof(_sensorIds[0].sensorId)-1);
            _sensorIds[_sensorIdCount].sensorId[sizeof(_sensorIds[0].sensorId)-1] = '\0';
            _sensorIdCount++;
        }
    }

    char tokenMask[12] = "(none)";
    if (_token[0]) snprintf(tokenMask, sizeof(tokenMask), "%.6s...", _token);
    Serial.printf("[OSM] boxId=%s sensors=%d token=%s\n",
                  _boxId, _sensorIdCount, tokenMask);
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

    // Build JSON array: [{sensor, value, createdAt}, ...]
    // Only include readings that have a mapped sensorId.
    // ~140 B per entry with createdAt; 160 keeps margin.
    size_t bodyLen = count * 160 + 32;
    // nothrow: the check below is only a check under -fno-exceptions, which
    // is how this firmware builds. A plain new[] that cannot allocate
    // aborts the device instead of returning null, so an export during a
    // heap squeeze became a reboot rather than a skipped batch. MqttExporter
    // says "like everything else in this tree" about its own nothrow
    // allocation; these two were the exceptions it did not know about.
    char*  body    = new (std::nothrow) char[bodyLen];
    if (!body) return false;

    size_t pos    = 0;
    int    mapped = 0;
    // Overflow-proof append: clamp so pos never advances past bodyLen (H1).
    auto appendOk = [&](int written) -> bool {
        size_t remaining = bodyLen - pos;
        if (written < 0 || (size_t)written >= remaining) {
            body[bodyLen - 1] = '\0';
            return false;
        }
        pos += written;
        return true;
    };

    bool full = appendOk(snprintf(body + pos, bodyLen - pos, "["));
    for (size_t i = 0; full && i < count; i++) {
        const char* sid = _lookupSensorId(readings[i].metric);
        if (!sid || sid[0] == '\0') continue;

        if (mapped > 0) {
            full = appendOk(snprintf(body + pos, bodyLen - pos, ","));
            if (!full) break;
        }
        // createdAt = the reading's own time, so the value lands when it was
        // measured — not when the (possibly much later, e.g. spooled) upload
        // happened. Only with a real wall-clock timestamp; otherwise the
        // server stamps arrival time.
        char created[40] = "";
        time_t ts = (time_t)readings[i].timestamp;
        if (readings[i].timestamp >= 1000000000u) {
            struct tm tmv;
            gmtime_r(&ts, &tmv);
            strftime(created, sizeof(created),
                     ",\"createdAt\":\"%Y-%m-%dT%H:%M:%SZ\"", &tmv);
        }
        full = appendOk(snprintf(body + pos, bodyLen - pos,
            "{\"sensor\":\"%s\",\"value\":\"%.6g\"%s}",
            sid, readings[i].value, created));
        mapped++;
    }
    if (full) appendOk(snprintf(body + pos, bodyLen - pos, "]"));

    bool ok = true;
    if (mapped > 0) {
        char url[128];
        snprintf(url, sizeof(url), "%s%s/data", API_BASE, _boxId);

        HTTPClient http;
        WiFiClient client;
        // R15: no CA store bundled — setInsecure() until 19.x rollout
        //       adds opt-in cert pinning in a follow-up phase
        
        http.begin(client, url);
        http.addHeader("Content-Type",  "application/json");
        char authHeader[80];
        snprintf(authHeader, sizeof(authHeader), "Bearer %s", _token);
        http.addHeader("Authorization", authHeader);

        int code = http.POST(body);
        ok = (code >= 200 && code < 300);
        if (!ok) Serial.printf("[OSM] POST failed code=%d\n", code);
        http.end();
    }

    delete[] body;
    return ok;
}
