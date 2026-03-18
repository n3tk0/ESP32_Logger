#include "MqttExporter.h"
#include "../core/Globals.h"  // config.deviceId

MqttExporter::~MqttExporter() {
    if (_client.connected()) _client.disconnect();
}

bool MqttExporter::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | false;
    if (!_enabled) return true;

    strncpy(_broker,      cfg["broker"]       | "localhost", sizeof(_broker)-1);
    _port = cfg["port"]   | 1883;
    strncpy(_topicPrefix, cfg["topic_prefix"] | "waterlogger", sizeof(_topicPrefix)-1);
    strncpy(_clientId,    cfg["client_id"]    | "", sizeof(_clientId)-1);
    strncpy(_username,    cfg["username"]      | "", sizeof(_username)-1);
    strncpy(_password,    cfg["password"]      | "", sizeof(_password)-1);
    _qos    = cfg["qos"]    | 0;
    _retain = cfg["retain"] | false;
    strncpy(_deviceId, config.deviceId, sizeof(_deviceId)-1);

    // Auto-generate client ID from deviceId if empty
    if (_clientId[0] == '\0') {
        snprintf(_clientId, sizeof(_clientId), "wl-%s", _deviceId);
    }

    _client.setClient(_wifiClient);
    _client.setServer(_broker, _port);
    _client.setKeepAlive(60);

    Serial.printf("[MQTT] broker=%s:%d prefix=%s\n",
                  _broker, _port, _topicPrefix);
    return true;
}

bool MqttExporter::_connect() {
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
        _client.loop(); // keep connection alive
        if (!_publish(readings[i])) allOk = false;
    }
    return allOk;
}
