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


// Bring WiFi up, retrying 3 times back-to-back before offering the setup portal.
static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        // Only flag it as running if it actually started: portalStartBackground()
        // refuses without basic-auth credentials, and setting the flag anyway
        // would have loop() calling portalHandleClient() on a server that was
        // never begun.
        if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);

    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[wifi] connecting to \"%s\" (attempt %d/3)", s_cfg.ssid, attempt);
        WiFi.begin(s_cfg.ssid, s_cfg.pass);

        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
                Serial.println(" timed out");
                break; // break the while loop to retry
            }
            delay(250);
            Serial.print('.');
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf(" ok, %s\n", WiFi.localIP().toString().c_str());
            if (!s_portalBgRunning) s_portalBgRunning = portalStartBackground(s_cfg);
            return true;
        }

        // Clean up before next attempt
        WiFi.disconnect();
        delay(1000);
    }

    WiFi.disconnect(true);
    Serial.println("[wifi] repeated failures — opening setup portal");
    s_portalBgRunning = false; // portalRun will stop the HTTP server on exit
    if (portalRun(s_cfg, PORTAL_TIMEOUT_MS)) {
        Serial.println("[cfg] saved, restarting");
        delay(200);
        ESP.restart();
    }
    
    // Portal timed out. Return false so loop can sleep.
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

    // THE SENSOR PROBE FIRST, THEN THE NETWORK, AND NEITHER GATES THE OTHER.
    //
    // Both orderings have been wrong here. With the WiFi check first, a node
    // that cannot reach its router never re-probes its sensor — ensureWifi()
    // spends three connect timeouts and can block in the portal, then returns
    // false, every cycle forever, and a power cut that took out both the
    // router and a cold breakout leaves the sensor unfound until someone
    // walks to it. With the sensor check first (as it shipped), a node whose
    // sensor was missing returned before ever reaching ensureWifi(), so it
    // never reconnected after a router reboot — and portalStartBackground(),
    // the one way to fix the wiring without a cable, is called from in there.
    //
    // The probe needs no network and costs a few milliseconds, so it goes
    // first and unconditionally; the network follows and is likewise not
    // conditional on the sensor. Only the POST needs both.
    if (!sensorsReady()) sensorsBegin(s_cfg);

    if (!ensureWifi()) return;
    if (!sensorsReady()) return;

    postReadings();
}
