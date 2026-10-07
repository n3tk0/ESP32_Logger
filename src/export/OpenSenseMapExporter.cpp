#include "OpenSenseMapExporter.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include <new>            // std::nothrow
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>          // isfinite
#include <memory>          // std::unique_ptr
#include <time.h>          // gmtime_r / strftime for createdAt

bool OpenSenseMapExporter::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | false;
    if (!_enabled) return true;

    strncpy(_boxId, cfg["box_id"]       | "", sizeof(_boxId)-1);
    strncpy(_token, cfg["access_token"] | "", sizeof(_token)-1);

    _sensorIdCount = 0;
    int over = 0;   // mappings past OSM_MAX_SENSORS: logged, not silently lost
    JsonObjectConst ids = cfg["sensor_ids"].as<JsonObjectConst>();
    for (auto kv : ids) {
        const char* sid = kv.value().as<const char*>();
        if (!sid || !*sid) continue;              // an empty field maps nothing
        if (_sensorIdCount >= OSM_MAX_SENSORS) { over++; continue; }
        SensorIdEntry& e = _sensorIds[_sensorIdCount++];
        strlcpy(e.metric,   kv.key().c_str(), sizeof(e.metric));
        strlcpy(e.sensorId, sid,              sizeof(e.sensorId));
    }

    char tokenMask[12] = "(none)";
    if (_token[0]) snprintf(tokenMask, sizeof(tokenMask), "%.6s...", _token);
    Log.printf("[OSM] boxId=%s sensors=%d token=%s\n",
                  _boxId, _sensorIdCount, tokenMask);
    if (over) Log.printf("[OSM] %d sensor IDs past the limit of %d ignored\n",
                            over, OSM_MAX_SENSORS);
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

// A reading openSenseMap can take: mapped to one of the box's sensors, and a
// real number. "%g" prints NaN/inf as "nan"/"inf", which the API answers with
// 422 for the WHOLE array, so one bad value would have cost every good one.
const char* OpenSenseMapExporter::_sendableId(const SensorReading& r) const {
    if (r.quality == QUALITY_ERROR || !isfinite(r.value)) return nullptr;
    const char* sid = _lookupSensorId(r.metric);
    return (sid && sid[0]) ? sid : nullptr;
}

bool OpenSenseMapExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || _boxId[0] == '\0' || count == 0) return true;

    // Size the body by what will actually be sent, not by the batch: the
    // latest-value snapshot carries every metric of every selected sensor,
    // and most of them usually have no openSenseMap sensor behind them.
    size_t mappedCount = 0;
    for (size_t i = 0; i < count; i++) if (_sendableId(readings[i])) mappedCount++;
    if (mappedCount == 0) return true;          // nothing for this box
    if (WiFi.status() != WL_CONNECTED) return false;

    // Build JSON array: [{sensor, value, createdAt}, ...]
    // Worst entry: 24-char id + "%.6g" + createdAt ≈ 110 B; 128 keeps margin.
    size_t bodyLen = mappedCount * 128 + 8;
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
        const char* sid = _sendableId(readings[i]);
        if (!sid) continue;

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
            "%s{\"sensor\":\"%s\",\"value\":\"%.6g\"%s}",
            mapped ? "," : "", sid, readings[i].value, created));
        if (full) mapped++;
    }
    // A body that did not fit is not valid JSON; never POST half of one.
    if (!full || !appendOk(snprintf(body + pos, bodyLen - pos, "]"))) {
        Log.println("[OSM] body overflow — batch dropped");
        delete[] body;
        return true;   // retrying the same batch would overflow the same way
    }

    char url[96];
    snprintf(url, sizeof(url), "%s%s/data", OSM_API_BASE, _boxId);

    // TLS or plain by OSM_API_BASE (see the header). The secure client is only
    // constructed for https://, so the plain path never touches mbedTLS.
    WiFiClient plain;
    std::unique_ptr<WiFiClientSecure> secure;
#if OSM_TLS
    secure.reset(new (std::nothrow) WiFiClientSecure);
    if (!secure) { delete[] body; return false; }
    // R15: no CA store bundled — setInsecure() until opt-in cert pinning.
    secure->setInsecure();
#endif

    HTTPClient http;   // declared after the clients: destroyed before them
    bool ok = false;
    if (http.begin(secure ? static_cast<WiFiClient&>(*secure) : plain, url)) {
        // Bounded so four attempts plus backoff stay inside the ExportTask
        // watchdog window (see ExportManager::_sendWithRetry).
        http.setTimeout(5000);
        http.addHeader("Content-Type",  "application/json");
        if (_token[0]) {
            char authHeader[80];
            snprintf(authHeader, sizeof(authHeader), "Bearer %s", _token);
            http.addHeader("Authorization", authHeader);
        }

        int code = http.POST((uint8_t*)body, pos);
        ok = (code >= 200 && code < 300);
        if (!ok && code >= 400 && code < 500 && code != 408 && code != 429) {
            // Rejected, not lost: a wrong box id/token (401/403/404) or data
            // the API refuses (422, e.g. a createdAt it deems in the future).
            // Sending it again gets the same answer, and spooling it would
            // replay the refusal in front of every later batch — so drop it.
            Log.printf("[OSM] POST rejected code=%d — batch dropped\n", code);
            ok = true;
        } else if (!ok) {
            Log.printf("[OSM] POST failed code=%d\n", code);
        }
        http.end();
    }

    delete[] body;
    return ok;
}
