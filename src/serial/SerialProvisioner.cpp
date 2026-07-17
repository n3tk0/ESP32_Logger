#include "SerialProvisioner.h"
#include <WiFi.h>
#include "../managers/OtaManager.h"   // tick() during blocking connect wait

SerialProvisioner serialProvisioner;

// ---------------------------------------------------------------------------
void SerialProvisioner::_respond(const char* json) {
    Serial.print(SERIAL_RESP_PREFIX);
    Serial.println(json);
}

void SerialProvisioner::_respondDoc(JsonDocument& doc) {
    // Serialize directly to Serial to avoid an intermediate String heap allocation.
    Serial.print(SERIAL_RESP_PREFIX);
    serializeJson(doc, Serial);
    Serial.println();
}

// ---------------------------------------------------------------------------
void SerialProvisioner::tick() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_len > 0) {
                _buf[_len] = '\0';
                _dispatch(_buf);
                _len = 0;
            }
        } else if (_len < BUF_SIZE - 1) {
            _buf[_len++] = c;
        } else {
            // Buffer overflow — silently drop and reset.
            _len = 0;
        }
    }
}

// ---------------------------------------------------------------------------
void SerialProvisioner::_dispatch(const char* line) {
    // Only parse lines that look like JSON objects.
    if (line[0] != '{') return;

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) {
        _respond("{\"ok\":false,\"err\":\"parse_error\"}");
        return;
    }

    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "ping") == 0) {
        _cmdPing();
    } else if (strcmp(cmd, "wifi_scan") == 0) {
        _cmdScan();
    } else if (strcmp(cmd, "wifi_connect") == 0) {
        const char* ssid = doc["ssid"] | "";
        const char* pass = doc["pass"] | "";
        _cmdConnect(ssid, pass);
    } else {
        _respond("{\"ok\":false,\"err\":\"unknown_cmd\"}");
    }
}

// ---------------------------------------------------------------------------
void SerialProvisioner::_cmdPing() {
    const char* modeStr = "off";
    switch (WiFi.getMode()) {
        case WIFI_MODE_AP:    modeStr = "ap";    break;
        case WIFI_MODE_STA:   modeStr = "sta";   break;
        case WIFI_MODE_APSTA: modeStr = "apsta"; break;
        default: break;
    }

    JsonDocument resp;
    resp["ok"]   = true;
    resp["mode"] = modeStr;
    if (WiFi.status() == WL_CONNECTED) {
        resp["ssid"] = WiFi.SSID();
        resp["ip"]   = WiFi.localIP().toString();
    } else if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
        resp["ap_ip"] = WiFi.softAPIP().toString();
    }
    _respondDoc(resp);
}

// ---------------------------------------------------------------------------
void SerialProvisioner::_cmdScan() {
    // Acknowledge immediately so the host knows scanning is in progress
    // (scan blocks for ~2-4 s and the host might time out otherwise).
    _respond("{\"ok\":\"scanning\"}");

    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);

    JsonDocument resp;
    if (n < 0) {
        resp["ok"]  = false;
        resp["err"] = "scan_failed";
    } else {
        resp["ok"] = true;
        JsonArray nets = resp["nets"].to<JsonArray>();
        // Sort by RSSI descending (strongest first) — insertion sort on cached
        // values.  WiFi.RSSI() involves IPC with the WiFi task so we read each
        // value once up front rather than calling it repeatedly inside the loop.
        int order[32];
        int rssis[32];
        int cnt = min(n, 32);
        for (int i = 0; i < cnt; i++) {
            order[i] = i;
            rssis[i] = WiFi.RSSI(i);
        }
        for (int i = 1; i < cnt; i++) {
            int keyIdx  = order[i];
            int keyRssi = rssis[keyIdx];
            int j = i - 1;
            while (j >= 0 && rssis[order[j]] < keyRssi) {
                order[j + 1] = order[j--];
            }
            order[j + 1] = keyIdx;
        }
        for (int i = 0; i < cnt; i++) {
            int idx = order[i];
            JsonObject net = nets.add<JsonObject>();
            net["ssid"] = WiFi.SSID(idx);
            net["rssi"] = WiFi.RSSI(idx);
            net["enc"]  = (WiFi.encryptionType(idx) == WIFI_AUTH_OPEN) ? 0 : 1;
        }
    }
    WiFi.scanDelete();
    _respondDoc(resp);
}

// ---------------------------------------------------------------------------
void SerialProvisioner::_cmdConnect(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) {
        _respond("{\"ok\":false,\"err\":\"no_ssid\"}");
        return;
    }

    Serial.printf("[SerialProvisioner] Connecting to '%s'…\n", ssid);

    // Switch to STA (keep AP running so we can recover via web if needed).
    WiFi.mode(WIFI_MODE_APSTA);
    WiFi.begin(ssid, (pass && pass[0]) ? pass : nullptr);

    // Wait up to 20 s for association.
    // • Call OtaManager::tick() each iteration so the 90-second OTA rollback
    //   confirmation window is not starved during this blocking wait.
    // • Exit early on terminal failure states so we don't spin the full timeout
    //   when the password is wrong or the SSID no longer exists.
    constexpr uint32_t TIMEOUT_MS = 20000;
    uint32_t start = millis();
    while (millis() - start < TIMEOUT_MS) {
        wl_status_t st = (wl_status_t)WiFi.status();
        if (st == WL_CONNECTED)      break;
        if (st == WL_CONNECT_FAILED) break;   // wrong password
        if (st == WL_NO_SSID_AVAIL)  break;   // SSID not found
        OtaManager::tick(millis());
        delay(500);
    }

    JsonDocument resp;
    if (WiFi.status() == WL_CONNECTED) {
        resp["ok"]   = true;
        resp["ip"]   = WiFi.localIP().toString();
        resp["gw"]   = WiFi.gatewayIP().toString();
        resp["ssid"] = WiFi.SSID();
        Serial.printf("[SerialProvisioner] Connected — IP %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        resp["ok"]  = false;
        resp["err"] = "timeout";
        // Revert to AP-only so the device is still accessible
        WiFi.disconnect(/*wifioff=*/false);
        WiFi.mode(WIFI_MODE_AP);
        Serial.println("[SerialProvisioner] Connection timed out — AP restored");
    }
    _respondDoc(resp);
}
