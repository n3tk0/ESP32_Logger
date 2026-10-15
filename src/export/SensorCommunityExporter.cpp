#include "SensorCommunityExporter.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include "../core/Globals.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "Throttle.h"      // parseRetryAfterS

bool SensorCommunityExporter::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]      | false;
    strncpy(_deviceId, config.deviceId, sizeof(_deviceId)-1);
    // The send interval is resolved and applied by ExportManager after init()
    // (own interval_ms, else export.defaults.interval_ms, never below
    // minIntervalMs()) — there is deliberately no second clock in here.
    Log.printf("[SC] deviceId=%s\n", _deviceId);
    return true;
}

// The five metrics the two pins carry; the rest of the snapshot is not sent.
bool SensorCommunityExporter::wants(const SensorReading& r) const {
    static const char* const M[] = { "pm25", "pm10", "temperature", "humidity", "pressure" };
    for (const char* m : M) if (strcmp(r.metric, m) == 0) return true;
    return false;
}

// The HTTP status, or a negative HTTPClient error. A 429 also sets the
// throttle (ExportManager then waits instead of retrying).
int SensorCommunityExporter::_postPin(const char* pin,
                                        const char* sensorName,
                                        const char* body)
{
    if (WiFi.status() != WL_CONNECTED) return -1;

    char sensorHeader[32];
    snprintf(sensorHeader, sizeof(sensorHeader), "esp32-%s", _deviceId);

    HTTPClient http;
    WiFiClientSecure secureClient;
    // R15: no CA store bundled — setInsecure() until 19.x rollout
    //       adds opt-in cert pinning in a follow-up phase
    secureClient.setInsecure();
    http.begin(secureClient, API_URL);
    // Bounded like openSenseMap's, inside the ExportTask watchdog window.
    http.setTimeout(5000);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("X-Pin",         pin);
    http.addHeader("X-Sensor",      sensorHeader);
    const char* headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);

    int code = http.POST(const_cast<char*>(body));
    if (code == 429) {
        _throttled   = true;
        _retryAfterS = parseRetryAfterS(http.header("Retry-After").c_str());
        Log.printf("[SC] pin=%s rate-limited (429) retry-after=%lus\n",
                   pin, (unsigned long)_retryAfterS);
    } else if (code != 201 && code != 200) {
        Log.printf("[SC] pin=%s POST failed code=%d\n", pin, code);
    }
    http.end();
    return code;
}

bool SensorCommunityExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled) return true;
    // No rate limit here: ExportManager only calls send() when the interval
    // (clamped to minIntervalMs()) has elapsed.
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
    // A retry resends every pin, so it is only worth one when none went
    // through: a pin that was accepted would be sent again seconds later,
    // against an API that wants 145 s between them. A 429 stops at once.
    int tried = 0, accepted = 0;

    // PM sensor (X-Pin 1) — SDS011 / PMS5003
    if (!isnan(pm25) && !isnan(pm10)) {
        snprintf(body, sizeof(body),
            "{\"software_version\":\"WaterLogger v5.0\","
            "\"sensordatavalues\":["
            "{\"value_type\":\"P1\",\"value\":\"%.2f\"},"
            "{\"value_type\":\"P2\",\"value\":\"%.2f\"}"
            "]}",
            pm10, pm25);
        const int code = _postPin("1", "SDS011", body);
        tried++;
        accepted += (code == 201 || code == 200);
        if (_throttled) return false;
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
        const int code = _postPin("11", "BME280", body);
        tried++;
        accepted += (code == 201 || code == 200);
    }

    return tried == 0 || accepted > 0;
}
