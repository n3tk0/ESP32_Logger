// ============================================================================
// ESP32_Logger sensor node — ESP8266 satellite
//
// Reads whatever sensors this build selected (see node_config.h) and POSTs
// the values to an ESP32_Logger collector's /api/ingest. That is the whole
// job: no storage, no display, and — outside the setup portal — no server and
// no listening port.
//
// Everything sensor-specific lives in sensors.cpp, so this file has no #ifdef
// per driver. Metric names and units match the collector's own plugins
// exactly, so a remote reading and a wired one are the same series shape.
//
// See README.md in this directory for wiring and setup.
// ============================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include "node_config.h"
#include "NodeSettings.h"
#include "ConfigPortal.h"
#include "sensors.h"

static NodeSettings s_cfg;
static uint32_t     s_lastPost   = 0;
static bool         s_postedOnce = false;

static bool         s_portalBgRunning = false;

// Consecutive failed association attempts. Drives how long the portal is
// offered before falling back to another retry.
static uint8_t s_wifiFailures = 0;

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[wifi] connecting to \"%s\"", s_cfg.ssid);
    WiFi.mode(WIFI_STA);
    // Persisting credentials to flash on every boot wears it out for no gain;
    // they live in /config.json.
    WiFi.persistent(false);
    WiFi.begin(s_cfg.ssid, s_cfg.pass);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println(" timed out");
            // Leave the radio off until the next attempt. A station stuck
            // mid-association draws more than an idle one and will not
            // recover on its own.
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
        Serial.print('.');
        yield();
    }
    Serial.printf(" ok, %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// Bring WiFi up, opening the setup portal when that keeps failing.
//
// The portal is time-boxed here, and that is the whole point: a router that
// reboots at 3 am must not leave the node parked in AP mode until someone
// notices. After PORTAL_TIMEOUT_MS it closes and the saved network is tried
// again, so the node self-heals — while still being reachable in that window
// if the credentials really did change.
static bool ensureWifi() {
    if (connectWifi()) {
        s_wifiFailures = 0;
        if (!s_portalBgRunning) {
            portalStartBackground(s_cfg);
            s_portalBgRunning = true;
        }
        return true;
    }

    // Three clean failures before offering the portal: transients can be cleared
    // and tearing the radio down to raise an AP costs a posting interval.
    if (++s_wifiFailures < 3) return false;

    Serial.println("[wifi] repeated failures — opening setup portal");
    s_portalBgRunning = false; // portalRun will stop the HTTP server on exit
    if (portalRun(s_cfg, PORTAL_TIMEOUT_MS)) {
        Serial.println("[cfg] saved, restarting");
        delay(200);
        ESP.restart();
    }
    s_wifiFailures = 0;   // give the saved network a fresh run of attempts
    return false;
}

// ---------------------------------------------------------------------------
// Post
// ---------------------------------------------------------------------------
static bool postReadings() {
    NodeReading vals[NODE_MAX_READINGS];
    const int n = sensorsRead(s_cfg, vals, NODE_MAX_READINGS);
    if (n == 0) {
        Serial.println("[sensor] nothing to send this cycle");
        return false;
    }

    JsonDocument doc;
    doc["node"] = s_cfg.nodeId;
    // No RTC and no NTP client on this node: send 0 and let the collector
    // stamp its own clock. It has NTP and is the one whose timeline the
    // readings are stored against.
    doc["ts"] = 0;

    JsonArray readings = doc["readings"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = readings.add<JsonObject>();
        o["metric"] = vals[i].metric;
        o["value"]  = vals[i].value;
        o["unit"]   = vals[i].unit;
    }

    String body;
    serializeJson(doc, body);

    WiFiClient  client;
    HTTPClient  http;
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%u/api/ingest",
             s_cfg.host, (unsigned)s_cfg.port);

    if (!http.begin(client, url)) {
        Serial.println("[post] http.begin failed");
        return false;
    }
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ingest-Token", s_cfg.token);
    if (s_cfg.basicUser[0] != '\0') {
        http.setAuthorization(s_cfg.basicUser, s_cfg.basicPass);
    }

    const int code = http.POST(body);
    if (code > 0) {
        Serial.printf("[post] %d metrics -> %d %s\n",
                      n, code, http.getString().c_str());
    } else {
        Serial.printf("[post] failed: %s\n", http.errorToString(code).c_str());
    }
    http.end();

    return code == 200;
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\nESP32_Logger sensor node");

    // Read the button before anything else claims GPIO0.
    const bool forcePortal = portalButtonHeld();

    settingsLoad(s_cfg);

    // Two reasons to run the portal with no timeout: there is nothing to fall
    // back to, or the user explicitly asked by holding FLASH through reset.
    // Both mean "wait for a human", so waiting indefinitely is correct.
    if (forcePortal || !s_cfg.isComplete()) {
        Serial.println(forcePortal ? "[portal] FLASH held at boot"
                                   : "[portal] no usable config");
        if (portalRun(s_cfg, 0)) {
            delay(200);
            ESP.restart();
        }
    }

    Serial.printf("node \"%s\" -> %s:%u every %lu s\n",
                  s_cfg.nodeId, s_cfg.host, (unsigned)s_cfg.port,
                  (unsigned long)(s_cfg.intervalMs / 1000UL));

    sensorsBegin(s_cfg);
    Serial.printf("sensors: %s\n", sensorsDescribe());
    ensureWifi();
}

void loop() {
    if (s_portalBgRunning) {
        portalHandleClient();
    }

    const uint32_t now = millis();

    // Unsigned subtraction, so the ~49-day millis() wrap is a non-event.
    if (s_postedOnce && (now - s_lastPost) < s_cfg.intervalMs) {
        delay(50);
        return;
    }
    s_lastPost   = now;
    s_postedOnce = true;

    // Retry rather than requiring a power cycle: a breakout on a cold balcony
    // can fail its first probe and answer a minute later. sensorsBegin() only
    // re-probes what is not already up.
    if (!sensorsReady()) sensorsBegin(s_cfg);
    if (!sensorsReady()) return;

    if (!ensureWifi()) return;

    postReadings();
}
