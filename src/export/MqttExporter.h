#pragma once
#include "IExporter.h"
#include "../drivers/MQTT_Mini.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// ============================================================================
// MqttExporter — publishes readings to MQTT broker.
//
// Config keys (from platform_config.json → export → mqtt):
//   broker, port (1883), topic_prefix ("waterlogger"),
//   client_id, username, password, qos (0), retain (false),
//   ha_discovery (false) — publish Home Assistant MQTT discovery payloads
//
// Topic format:
//   {prefix}/device/{deviceId}/sensor/{sensorId}/{metric}
//   Payload: {"ts":..,"value":..,"unit":..,"q":..}
//
// HA discovery topics:
//   homeassistant/sensor/{deviceId}_{sensorId}_{metric}/config
// ============================================================================
/// What the broker is told to expect, and what _connect() measures staleness
/// against.
///
/// COMFORTABLY LONGER THAN THE EXPORT INTERVAL, and that is the whole point of
/// the number. It was 60 s, which is also the default aggregation interval — so
/// each export arrived at roughly the moment the socket was declared stale, and
/// the connection was torn down and re-handshaked nearly every cycle. That is
/// the TLS cost the persistent socket exists to avoid, paid on a timer.
///
/// At 300 s a device exporting every minute never trips the check: its own
/// publishes are the traffic that keeps the window open. One that has been
/// quiet for five minutes rebuilds, which is right — after that long the broker
/// may well have dropped it, and a publish into a closed socket succeeds
/// silently.
#ifndef MQTT_KEEPALIVE_S
#  define MQTT_KEEPALIVE_S 300
#endif

class MqttExporter : public IExporter {
public:
    ~MqttExporter();

    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "mqtt"; }
    bool        isEnabled() const override { return _enabled; }

    // Publish HA MQTT discovery payloads for all known sensors.
    // Call once after init() (or on demand via API).
    void        publishHaDiscovery();

private:
    bool _connect();
    bool _publish(const SensorReading& r);
    bool _publishDiscoveryOne(const char* sensorId, const char* sensorName,
                              const char* metric,   const char* unit,
                              const char* deviceClass);

    Client*      _stream = nullptr;
    MQTT_Mini    _client;

    char     _broker[65]      = {};
    uint16_t _port            = 1883;
    char     _topicPrefix[33] = "waterlogger";
    char     _clientId[33]    = {};
    char     _username[33]    = {};
    char     _password[65]    = {};
    uint8_t  _qos             = 0;
    bool     _retain          = false;
    bool     _haDiscovery     = false;
    char     _deviceId[13]    = {};
    bool     _useTls          = false;
};
