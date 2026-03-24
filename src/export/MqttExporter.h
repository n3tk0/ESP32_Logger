#pragma once
#include "IExporter.h"
#include "../drivers/MQTT_Mini.h"
#include <WiFiClient.h>

// ============================================================================
// MqttExporter — publishes readings to MQTT broker.
//
// Config keys (from platform_config.json → export → mqtt):
//   broker, port (1883), topic_prefix ("waterlogger"),
//   client_id, username, password, qos (0), retain (false)
//
// Topic format:
//   {prefix}/device/{deviceId}/sensor/{sensorId}/{metric}
//   Payload: {"ts":..,"value":..,"unit":..,"q":..}
// ============================================================================
class MqttExporter : public IExporter {
public:
    ~MqttExporter();

    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "mqtt"; }
    bool        isEnabled() const override { return _enabled; }

private:
    bool _connect();
    bool _publish(const SensorReading& r);

    WiFiClient   _wifiClient;
    MQTT_Mini    _client;

    char     _broker[65]      = {};
    uint16_t _port            = 1883;
    char     _topicPrefix[33] = "waterlogger";
    char     _clientId[33]    = {};
    char     _username[33]    = {};
    char     _password[65]    = {};
    uint8_t  _qos             = 0;
    bool     _retain          = false;
    char     _deviceId[13]    = {};
};
