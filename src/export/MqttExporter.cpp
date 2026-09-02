#include "MqttExporter.h"
#include <new>                        // new (std::nothrow) — this part aborts otherwise
#include "../core/Globals.h"  // config.deviceId
#include "../sensors/SensorManager.h"  // publishHaDiscovery()

MqttExporter::~MqttExporter() {
    if (_client.connected()) _client.disconnect();
    if (_stream) {
        delete _stream;
        _stream = nullptr;
    }
}

bool MqttExporter::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | false;
    if (!_enabled) {
        // Turning the exporter off has to release the socket, not merely stop
        // using it. The connection used to live on the stack for the length of
        // one send, so there was nothing to release; now it persists, and a
        // disabled exporter that kept an open TLS session would hold tens of
        // kilobytes of mbedtls state for a feature the user just switched off.
        if (_stream) {
            if (_client.connected()) _client.disconnect();
            delete _stream;
            _stream = nullptr;
        }
        return true;
    }

    strncpy(_broker,      cfg["broker"]       | "localhost", sizeof(_broker)-1);
    _port = cfg["port"]   | 1883;
    strncpy(_topicPrefix, cfg["topic_prefix"] | "waterlogger", sizeof(_topicPrefix)-1);
    strncpy(_clientId,    cfg["client_id"]    | "", sizeof(_clientId)-1);
    strncpy(_username,    cfg["username"]      | "", sizeof(_username)-1);
    strncpy(_password,    cfg["password"]      | "", sizeof(_password)-1);
    _qos         = cfg["qos"]          | 0;
    _retain      = cfg["retain"]       | false;
    _haDiscovery = cfg["ha_discovery"] | false;
    _useTls      = cfg["use_tls"]      | false;
    strncpy(_deviceId, config.deviceId, sizeof(_deviceId)-1);

    // MQTT_Mini only supports QoS 0 (publish-only). Zero silently.
    if (_qos > 0) {
        Serial.printf("[MQTT] WARNING: qos=%u requested but only QoS 0 is supported; "
                      "using QoS 0.\n", _qos);
        _qos = 0;
    }

    // Auto-generate client ID from deviceId if empty
    if (_clientId[0] == '\0') {
        snprintf(_clientId, sizeof(_clientId), "wl-%s", _deviceId);
    }

    // The socket outlives a single send now, so it is owned here rather than
    // built on the stack per publish. Replacing it means tearing the old one
    // down first — a reconfigure that flips use_tls would otherwise leave a
    // connected socket with nothing pointing at it.
    if (_stream) {
        if (_client.connected()) _client.disconnect();
        delete _stream;
        _stream = nullptr;
    }

    // new (std::nothrow), like everything else in this tree. WiFiClientSecure
    // brings an mbedtls context with it, this part runs close to its heap
    // limit, and a plain `new` under -fno-exceptions does not fail — it aborts,
    // which is a reboot loop rather than an exporter that reports it is off.
    if (_useTls) {
        WiFiClientSecure* sec = new (std::nothrow) WiFiClientSecure();
        // R15: no CA store is bundled, so the certificate is not verified.
        // Encryption without authentication still keeps credentials off the
        // wire in clear, which is what this buys until cert pinning lands.
        if (sec) sec->setInsecure();
        _stream = sec;
    } else {
        _stream = new (std::nothrow) WiFiClient();
    }
    if (!_stream) {
        Serial.println("[MQTT] out of memory allocating the socket — exporter disabled");
        _enabled = false;
        return false;
    }

    _client.setClient(*_stream);
    _client.setServer(_broker, _port);
    _client.setKeepAlive(MQTT_KEEPALIVE_S);

    Serial.printf("[MQTT] broker=%s:%d prefix=%s tls=%s\n",
                  _broker, _port, _topicPrefix, _useTls ? "yes" : "no");
    return true;
}

bool MqttExporter::_connect() {
    // A CONNECTION THE BROKER HAS ALREADY GIVEN UP ON STILL READS AS CONNECTED
    // HERE. connected() asks the TCP layer, which does not learn of a
    // broker-side close until a write fails or a FIN arrives and is noticed —
    // so publish() returns true and the readings go nowhere, silently.
    //
    // That could not happen while every send connected and disconnected around
    // itself. Now that the socket persists between exports, an export interval
    // longer than the keepalive is the ordinary case, not an edge one: this
    // device exports on the aggregation interval, which defaults to 60 s.
    //
    // So a connection past its keepalive is torn down and rebuilt rather than
    // trusted. The handshake cost returns only in the case where the saving was
    // never real.
    if (_client.connected() && _client.keepAliveExpired()) {
        Serial.println("[MQTT] connection idle past the keepalive — reconnecting");
        _client.disconnect();
        if (_stream) _stream->stop();
    }

    if (_client.connected()) return true;

    bool ok;
    if (_username[0] != '\0') {
        ok = _client.connect(_clientId, _username, _password);
    } else {
        ok = _client.connect(_clientId);
    }
    if (!ok) {
        Serial.printf("[MQTT] connect failed, rc=%d\n", _client.state());
    }
    return ok;
}

bool MqttExporter::_publish(const SensorReading& r) {
    char topic[128];
    snprintf(topic, sizeof(topic),
             "%s/device/%s/sensor/%s/%s",
             _topicPrefix, _deviceId, r.sensorId, r.metric);

    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"ts\":%lu,\"value\":%.4g,\"unit\":\"%s\",\"q\":%u}",
             (unsigned long)r.timestamp, r.value, r.unit, (unsigned)r.quality);

    return _client.publish(topic, payload, _retain);
}

bool MqttExporter::send(const SensorReading* readings, size_t count) {
    if (!_enabled || count == 0) return true;

    if (!_connect()) return false;
    bool allOk = true;
    for (size_t i = 0; i < count; i++) {
        _client.loop();
        if (!_publish(readings[i])) allOk = false;
    }
    return allOk;
}

// ---------------------------------------------------------------------------
// HA MQTT Discovery
// Metric → device_class mapping (HA-known classes only)
// ---------------------------------------------------------------------------
static const char* _haDeviceClass(const char* metric) {
    if (strstr(metric, "temperature"))  return "temperature";
    if (strstr(metric, "humidity"))     return "humidity";
    if (strstr(metric, "pressure"))     return "atmospheric_pressure";
    if (strstr(metric, "co2"))          return "carbon_dioxide";
    if (strstr(metric, "tvoc"))         return "volatile_organic_compounds";
    if (strstr(metric, "lux"))          return "illuminance";
    if (strstr(metric, "pm25"))         return "pm25";
    if (strstr(metric, "pm10"))         return "pm10";
    if (strstr(metric, "pm1"))          return "pm1";
    if (strstr(metric, "voltage"))      return "voltage";
    if (strstr(metric, "current"))      return "current";
    if (strstr(metric, "moisture"))     return "moisture";
    if (strstr(metric, "distance"))     return "distance";
    if (strstr(metric, "flow"))         return "volume_flow_rate";
    return "";  // no device_class for unknown metrics
}

bool MqttExporter::_publishDiscoveryOne(const char* sensorId, const char* sensorName,
                                         const char* metric,   const char* unit,
                                         const char* deviceClass) {
    char topic[128];
    snprintf(topic, sizeof(topic),
             "homeassistant/sensor/%s_%s_%s/config",
             _deviceId, sensorId, metric);

    // State topic: same format as normal publish
    char stateTopic[128];
    snprintf(stateTopic, sizeof(stateTopic),
             "%s/device/%s/sensor/%s/%s",
             _topicPrefix, _deviceId, sensorId, metric);

    char uid[64];
    snprintf(uid, sizeof(uid), "wl_%s_%s_%s", _deviceId, sensorId, metric);

    char name[64];
    snprintf(name, sizeof(name), "%s %s", sensorName, metric);

    // Compose payload
    JsonDocument doc;
    doc["name"]          = name;
    doc["state_topic"]   = stateTopic;
    doc["value_template"] = "{{ value_json.value }}";
    doc["unique_id"]     = uid;
    if (unit && unit[0])        doc["unit_of_measurement"] = unit;
    if (deviceClass && deviceClass[0]) doc["device_class"]= deviceClass;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = String("wl_") + _deviceId;
    dev["name"]           = String(config.deviceName[0] ? config.deviceName : "Water Logger");
    dev["model"]          = "ESP32-C3 Logger";
    dev["manufacturer"]   = "DIY";

    char payload[512];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    if (len >= sizeof(payload)) return false;  // payload too large

    return _client.publish(topic, payload, /*retain=*/true);
}

void MqttExporter::publishHaDiscovery() {
    if (!_enabled || !_haDiscovery) return;

    if (!_connect()) {
        Serial.println("[MQTT] HA discovery: not connected");
        return;
    }
    Serial.println("[MQTT] Publishing HA discovery payloads…");
    int n = sensorManager.count();
    int published = 0;
    for (int i = 0; i < n; i++) {
        ISensor* s = sensorManager.get(i);
        if (!s || !s->isEnabled()) continue;
        const char* mNames[8] = {};
        int mCount = s->getMetrics(mNames, 8);
        for (int m = 0; m < mCount; m++) {
            const char* dc = _haDeviceClass(mNames[m]);
            if (_publishDiscoveryOne(s->getId(), s->getName(), mNames[m], "", dc)) {
                published++;
            }
            _client.loop();
            delay(20);  // give broker time to process
        }
    }
    Serial.printf("[MQTT] HA discovery: %d topics published\n", published);
}
