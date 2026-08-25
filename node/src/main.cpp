// ============================================================================
// ESP32_Logger sensor node — ESP8266 + BME280/BMP280
//
// Reads one I2C environment sensor and POSTs the values to an ESP32_Logger
// collector's /api/ingest. That is the whole job: no storage, no display, and
// — outside the setup portal — no server and no listening port.
//
// Metric names and units match the collector's own BME280 plugin exactly
// ("temperature"/C, "humidity"/%, "pressure"/hPa) so a remote reading and a
// wired one are the same series shape downstream.
//
// See README.md in this directory for wiring and setup.
// ============================================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <ArduinoJson.h>

#include "node_config.h"
#include "NodeSettings.h"
#include "ConfigPortal.h"

// The collector's driver, used unmodified. It speaks only Wire, so it is as
// portable as the bus is — and sharing it means the compensation maths cannot
// drift between the two devices.
#include "src/drivers/BME280_Mini.h"

static BME280_Mini  s_bmx;
static NodeSettings s_cfg;
static bool         s_sensorReady = false;
static uint32_t     s_lastPost    = 0;
static bool         s_postedOnce  = false;

// Consecutive failed association attempts. Drives how long the portal is
// offered before falling back to another retry.
static uint8_t s_wifiFailures = 0;

// ---------------------------------------------------------------------------
// Sea-level pressure
// ---------------------------------------------------------------------------
// Station pressure falls about 12 Pa per metre of altitude near sea level, so
// a node 300 m up reads ~35 hPa below what a forecast quotes. The barometric
// formula converts one to the other; without it, comparing the node's reading
// against a forecast's "1013 hPa" is comparing two different quantities.
//
// Both are published when altitude is set, because they answer different
// questions: station pressure is what this box actually experiences (the one
// to trend), sea-level pressure is what compares against everyone else.
static float toSeaLevel(float stationHpa, float tempC, float altitudeM) {
    if (altitudeM == 0.0f) return NAN;
    return stationHpa * powf(1.0f - (0.0065f * altitudeM)
                                    / (tempC + 0.0065f * altitudeM + 273.15f),
                             -5.257f);
}

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
    if (connectWifi()) { s_wifiFailures = 0; return true; }

    // Two clean failures before offering the portal: one is a transient the
    // next cycle usually clears, and tearing the radio down to raise an AP
    // costs a posting interval.
    if (++s_wifiFailures < 2) return false;

    Serial.println("[wifi] repeated failures — opening setup portal");
    if (portalRun(s_cfg, PORTAL_TIMEOUT_MS)) {
        Serial.println("[cfg] saved, restarting");
        delay(200);
        ESP.restart();
    }
    s_wifiFailures = 0;   // give the saved network a fresh run of attempts
    return false;
}

// ---------------------------------------------------------------------------
// Sensor
// ---------------------------------------------------------------------------
static bool initSensor() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    // Breakouts strap SDO either way; try the configured address first, then
    // the other one, so a board that shipped as 0x77 still works untouched.
    const uint8_t candidates[2] = { BMX280_ADDR,
                                    (uint8_t)(BMX280_ADDR == 0x76 ? 0x77 : 0x76) };
    for (uint8_t i = 0; i < 2; i++) {
        if (s_bmx.begin(candidates[i], &Wire)) {
            Serial.printf("[sensor] %s at 0x%02X (chip 0x%02X)\n",
                          s_bmx.isBME280() ? "BME280" : "BMP280",
                          candidates[i], s_bmx.chipId());
            return true;
        }
    }
    Serial.println("[sensor] no BME280/BMP280 found on either address");
    return false;
}

// ---------------------------------------------------------------------------
// Post
// ---------------------------------------------------------------------------
static void addReading(JsonArray arr, const char* metric, float value,
                       const char* unit) {
    if (!isfinite(value)) return;   // omit rather than send a NaN the JSON cannot carry
    JsonObject o = arr.add<JsonObject>();
    o["metric"] = metric;
    o["value"]  = value;
    o["unit"]   = unit;
}

static bool postReadings() {
    const float tempC       = s_bmx.readTemperature();
    const float pressurePa  = s_bmx.readPressure();
    const float humidityPct = s_bmx.readHumidity();   // NAN on a BMP280

    if (!isfinite(tempC) && !isfinite(pressurePa)) {
        Serial.println("[sensor] read failed, skipping post");
        return false;
    }

    JsonDocument doc;
    doc["node"] = s_cfg.nodeId;
    // No RTC and no NTP client on this node: send 0 and let the collector
    // stamp its own clock at drain. It has NTP and is the one whose timeline
    // the readings are stored against.
    doc["ts"] = 0;

    JsonArray readings = doc["readings"].to<JsonArray>();
    addReading(readings, "temperature", tempC, "C");
    addReading(readings, "humidity",    humidityPct, "%");

    const float hPa = isfinite(pressurePa) ? pressurePa / 100.0f : NAN;
    addReading(readings, "pressure", hPa, "hPa");
    addReading(readings, "pressure_sea",
               toSeaLevel(hPa, tempC, s_cfg.altitudeM), "hPa");

    if (readings.size() == 0) {
        Serial.println("[sensor] nothing finite to send");
        return false;
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
        Serial.printf("[post] %d %s\n", code, http.getString().c_str());
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

    s_sensorReady = initSensor();
    ensureWifi();
}

void loop() {
    const uint32_t now = millis();

    // Unsigned subtraction, so the ~49-day millis() wrap is a non-event.
    if (s_postedOnce && (now - s_lastPost) < s_cfg.intervalMs) {
        delay(50);
        return;
    }
    s_lastPost   = now;
    s_postedOnce = true;

    // Retry the sensor rather than requiring a power cycle: a breakout on a
    // cold balcony can fail its first probe and come back a minute later.
    if (!s_sensorReady) s_sensorReady = initSensor();
    if (!s_sensorReady) return;

    if (!ensureWifi()) return;

    postReadings();
}
