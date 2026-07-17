#include "HttpExporter.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

static bool _isValidHeaderName(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static bool _isValidHeaderValue(const char* s) {
    if (!s) return false;
    for (const char* p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') return false;
    }
    return true;
}

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
            const char* k = kv.key().c_str();
            const char* v = kv.value().as<const char*>() ?: "";
            if (!_isValidHeaderName(k) || !_isValidHeaderValue(v)) {
                Serial.printf("[HTTP] rejected header '%s': invalid name or CRLF in value\n", k);
                continue;
            }
            strncpy(_hdrKeys[_hdrCount], k, sizeof(_hdrKeys[0])-1);
            _hdrKeys[_hdrCount][sizeof(_hdrKeys[0])-1] = '\0';
            strncpy(_hdrVals[_hdrCount], v, sizeof(_hdrVals[0])-1);
            _hdrVals[_hdrCount][sizeof(_hdrVals[0])-1] = '\0';
            _hdrCount++;
        }
    }

    Serial.printf("[HTTP] url=%s method=%s\n", _url, _method);
    return true;
}

bool HttpExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || count == 0 || _url[0] == '\0') return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    // Build JSON array body.
    // Worst-case per reading = 63B fixed template + ts(10) + sensorId(16) +
    // sensorType(11) + metric(15) + value(~12 for %.4g) + unit(11) +
    // quality(3) = ~139B. Budget 160B/reading gives margin, plus 32B for the
    // "[" / "]" framing and NUL. (Verified: tests/host/test_httpexporter_bufsize.cpp.)
    static const size_t BYTES_PER_READING = 160;
    size_t bodyLen = count * BYTES_PER_READING + 32;
    char*  body    = new char[bodyLen];
    if (!body) return false;

    // Overflow-proof append helper: after each snprintf, clamp on error or when
    // the result would not fit the remaining space, so pos never exceeds bodyLen.
    // Returns false when the append had to be truncated.
    size_t pos = 0;
    auto appendOk = [&](int written) -> bool {
        size_t remaining = bodyLen - pos;
        if (written < 0 || (size_t)written >= remaining) {
            body[bodyLen - 1] = '\0';   // keep buffer NUL-terminated
            return false;
        }
        pos += written;
        return true;
    };

    bool full = appendOk(snprintf(body + pos, bodyLen - pos, "["));
    for (size_t i = 0; full && i < count; i++) {
        const SensorReading& r = readings[i];
        if (i > 0) {
            full = appendOk(snprintf(body + pos, bodyLen - pos, ","));
            if (!full) break;
        }
        full = appendOk(snprintf(body + pos, bodyLen - pos,
            "{\"ts\":%lu,\"id\":\"%s\",\"sensor\":\"%s\","
            "\"metric\":\"%s\",\"value\":%.4g,\"unit\":\"%s\",\"q\":%u}",
            (unsigned long)r.timestamp, r.sensorId, r.sensorType,
            r.metric, r.value, r.unit, (unsigned)r.quality));
    }
    if (full) appendOk(snprintf(body + pos, bodyLen - pos, "]"));

    HTTPClient http;
    bool isHttps = strncmp(_url, "https://", 8) == 0;
    WiFiClientSecure secureClient;
    if (isHttps) {
        // R15: no CA store bundled — setInsecure() until 19.x rollout
        //       adds opt-in cert pinning in a follow-up phase
        secureClient.setInsecure();
        http.begin(secureClient, _url);
    } else {
        http.begin(_url);
    }
    http.addHeader("Content-Type", "application/json");
    for (int i = 0; i < _hdrCount; i++) {
        http.addHeader(_hdrKeys[i], _hdrVals[i]);
    }

    int code = http.POST(body);
    bool ok  = (code >= 200 && code < 300);
    if (!ok) {
        Serial.printf("[HTTP] POST failed, code=%d\n", code);
    } else {
        Serial.printf("[HTTP] sent to %s\n", _url);
    }
    http.end();
    delete[] body;
    return ok;
}
