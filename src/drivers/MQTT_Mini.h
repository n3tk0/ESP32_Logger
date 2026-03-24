// ============================================================================
// MQTT_Mini — Minimal MQTT 3.1.1 publish-only client (no PubSubClient)
// Supports: CONNECT (with optional auth), PUBLISH (QoS 0), DISCONNECT
// ============================================================================
#pragma once
#include <Arduino.h>
#include <WiFiClient.h>

class MQTT_Mini {
public:
    void setClient(WiFiClient& client) { _tcp = &client; }
    void setServer(const char* broker, uint16_t port) {
        _broker = broker;
        _port   = port;
    }
    void setKeepAlive(uint16_t sec) { _keepAlive = sec; }

    bool connected() { return _tcp && _tcp->connected() && _connected; }

    // Connect without credentials
    bool connect(const char* clientId) {
        return connect(clientId, nullptr, nullptr);
    }

    // Connect with optional credentials
    bool connect(const char* clientId, const char* user, const char* pass) {
        if (!_tcp || !_broker) return false;
        _connected = false;

        if (!_tcp->connect(_broker, _port)) {
            _state = -2;  // connection refused
            return false;
        }

        // Build CONNECT packet
        uint8_t buf[256];
        uint8_t* p = buf;

        // Variable header
        uint8_t var[10];
        var[0] = 0; var[1] = 4;  // protocol name length
        var[2] = 'M'; var[3] = 'Q'; var[4] = 'T'; var[5] = 'T';
        var[6] = 4;  // protocol level (MQTT 3.1.1)

        uint8_t flags = 0x02;  // clean session
        bool hasUser = (user && user[0] != '\0');
        bool hasPass = (pass && pass[0] != '\0');
        if (hasUser) flags |= 0x80;
        if (hasPass) flags |= 0x40;
        var[7] = flags;

        var[8] = (_keepAlive >> 8) & 0xFF;
        var[9] = _keepAlive & 0xFF;

        // Calculate remaining length
        uint16_t clientIdLen = strlen(clientId);
        uint16_t userLen = hasUser ? strlen(user) : 0;
        uint16_t passLen = hasPass ? strlen(pass) : 0;
        uint32_t remaining = 10 + 2 + clientIdLen;
        if (hasUser) remaining += 2 + userLen;
        if (hasPass) remaining += 2 + passLen;

        if (remaining + 5 > sizeof(buf)) { _state = -4; return false; }

        // Fixed header: CONNECT = 0x10
        *p++ = 0x10;
        p += _encodeLength(p, remaining);

        // Variable header
        memcpy(p, var, 10); p += 10;

        // Payload: client ID
        *p++ = (clientIdLen >> 8) & 0xFF;
        *p++ = clientIdLen & 0xFF;
        memcpy(p, clientId, clientIdLen); p += clientIdLen;

        // Username
        if (hasUser) {
            *p++ = (userLen >> 8) & 0xFF;
            *p++ = userLen & 0xFF;
            memcpy(p, user, userLen); p += userLen;
        }
        // Password
        if (hasPass) {
            *p++ = (passLen >> 8) & 0xFF;
            *p++ = passLen & 0xFF;
            memcpy(p, pass, passLen); p += passLen;
        }

        size_t total = p - buf;
        if (_tcp->write(buf, total) != total) {
            _state = -3;
            return false;
        }
        _tcp->flush();

        // Wait for CONNACK (4 bytes: 0x20, 0x02, flags, rc)
        uint32_t start = millis();
        while (_tcp->available() < 4 && (millis() - start) < 5000) {
            delay(10);
        }
        if (_tcp->available() < 4) {
            _tcp->stop();
            _state = -3;
            return false;
        }

        uint8_t connack[4];
        _tcp->readBytes(connack, 4);
        if (connack[0] != 0x20 || connack[3] != 0x00) {
            _state = connack[3];
            _tcp->stop();
            return false;
        }

        _connected = true;
        _state = 0;
        return true;
    }

    // Publish QoS 0 message
    bool publish(const char* topic, const char* payload, bool retain = false) {
        if (!connected()) return false;

        uint16_t topicLen = strlen(topic);
        uint16_t payloadLen = strlen(payload);
        uint32_t remaining = 2 + topicLen + payloadLen;

        // Fixed header: PUBLISH = 0x30, retain bit
        uint8_t hdr[5];
        uint8_t* p = hdr;
        *p++ = 0x30 | (retain ? 0x01 : 0x00);
        p += _encodeLength(p, remaining);
        size_t hdrLen = p - hdr;

        // Topic length + topic
        uint8_t topicHdr[2] = {
            (uint8_t)((topicLen >> 8) & 0xFF),
            (uint8_t)(topicLen & 0xFF)
        };

        // Send in parts to avoid large buffer allocation
        if (_tcp->write(hdr, hdrLen) != hdrLen) goto fail;
        if (_tcp->write(topicHdr, 2) != 2) goto fail;
        if (_tcp->write((const uint8_t*)topic, topicLen) != topicLen) goto fail;
        if (payloadLen > 0) {
            if (_tcp->write((const uint8_t*)payload, payloadLen) != payloadLen) goto fail;
        }
        _tcp->flush();
        return true;

    fail:
        _connected = false;
        return false;
    }

    void loop() {
        // Drain any incoming data (we don't subscribe, but broker may send PINGRESP)
        if (_tcp) {
            while (_tcp->available()) _tcp->read();
        }
    }

    void disconnect() {
        if (_tcp && _tcp->connected()) {
            uint8_t pkt[] = { 0xE0, 0x00 };  // DISCONNECT
            _tcp->write(pkt, 2);
            _tcp->flush();
            _tcp->stop();
        }
        _connected = false;
    }

    int state() const { return _state; }

private:
    WiFiClient* _tcp = nullptr;
    const char* _broker = nullptr;
    uint16_t    _port = 1883;
    uint16_t    _keepAlive = 60;
    bool        _connected = false;
    int         _state = 0;  // 0=ok, negative=error, positive=CONNACK rc

    // Encode MQTT remaining length (1-4 bytes)
    static int _encodeLength(uint8_t* buf, uint32_t len) {
        int i = 0;
        do {
            uint8_t b = len & 0x7F;
            len >>= 7;
            if (len > 0) b |= 0x80;
            buf[i++] = b;
        } while (len > 0 && i < 4);
        return i;
    }
};
