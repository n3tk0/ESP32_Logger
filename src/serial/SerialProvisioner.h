#pragma once
// ============================================================================
// SerialProvisioner — WiFi provisioning via USB-CDC / UART without AP.
//
// Listens on the hardware Serial for newline-terminated JSON commands and
// replies with JSON prefixed by SERIAL_RESP_PREFIX so the host tool
// (tools/deploy.py  [W] option) can distinguish responses from regular
// firmware log output.
//
// Supported commands:
//   {"cmd":"ping"}
//     → {"ok":true,"mode":"ap"|"sta"|"apsta"|"off","ssid":"...","ip":"..."}
//
//   {"cmd":"wifi_scan"}
//     → {"ok":true,"nets":[{"ssid":"...","rssi":-65,"enc":0|1},...]}
//       enc: 0 = open, 1 = password required
//
//   {"cmd":"wifi_connect","ssid":"MyNet","pass":"secret"}
//     → {"ok":true,"ip":"192.168.1.42","gw":"192.168.1.1","ssid":"MyNet"}
//     → {"ok":false,"err":"timeout"|"no_ssid"}
//
// Usage
// ─────
//   Call serialProvisioner.tick() from loop() — it is non-blocking except
//   during an active wifi_scan or wifi_connect command (~2 s and ≤15 s
//   respectively).  These operations are user-triggered so the temporary
//   block of loop() is acceptable.
//
// ============================================================================

#include <Arduino.h>
#include <ArduinoJson.h>

// Response lines sent to the host are prefixed with this marker so deploy.py
// can filter them out from normal log output.
constexpr const char* SERIAL_RESP_PREFIX = ">>SP<<";

class SerialProvisioner {
public:
    // Call once per loop() iteration.  Drains Serial.available() and
    // dispatches complete lines as JSON commands.
    void tick();

private:
    static constexpr size_t BUF_SIZE = 256;
    char   _buf[BUF_SIZE] = {};
    size_t _len           = 0;

    void _dispatch(const char* line);

    void _cmdPing();
    void _cmdScan();
    void _cmdConnect(const char* ssid, const char* pass);

    // Emit a JSON response string (with prefix + newline).
    void _respond(const char* json);
    void _respondDoc(JsonDocument& doc);
};

extern SerialProvisioner serialProvisioner;
