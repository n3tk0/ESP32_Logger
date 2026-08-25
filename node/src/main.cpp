// ============================================================================
// ESP32_Logger sensor node — ESP8266 + BME280/BMP280
//
// Reads one I2C environment sensor and POSTs the values to an ESP32_Logger
// collector's /api/ingest. That is the whole job: no web server, no
// filesystem, no local storage, no display. Anything that wants to look at
// this node's data looks at the collector.
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

// The collector's driver, used unmodified. It speaks only Wire, so it is as
// portable as the bus is — and sharing it means the compensation maths cannot
// drift between the two devices.
#include "src/drivers/BME280_Mini.h"

static BME280_Mini s_bmx;
static bool        s_sensorReady = false;
static uint32_t    s_lastPost    = 0;

// Posting starts immediately on boot rather than after one full interval:
// the first thing you want after plugging a node in is to see it appear.
static bool s_postedOnce = false;

// ---------------------------------------------------------------------------
// Sea-level pressure
// ---------------------------------------------------------------------------
// Station pressure falls about 12 Pa per metre of altitude near sea level, so
// a node 300 m up reads ~35 hPa below what a forecast quotes. The barometric
// formula converts one to the other; without it, comparing the node's reading
// against a forecast's "1013 hPa" is comparing two different quantities.
//
// Both are published when ALTITUDE_M is set, because they answer different
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
static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    // Persisting credentials to flash on every boot wears it out for no gain;
    // they are compiled in here.
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

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
    doc["node"] = NODE_ID;
    // No RTC and no NTP client on this node: send 0 and let the collector
    // stamp its own clock at drain. It has NTP and is the one whose timeline
    // the readings are stored against.
    doc["ts"] = 0;

    JsonArray readings = doc["readings"].to<JsonArray>();
    addReading(readings, "temperature", tempC, "C");
    addReading(readings, "humidity",    humidityPct, "%");

    const float hPa = isfinite(pressurePa) ? pressurePa / 100.0f : NAN;
    addReading(readings, "pressure", hPa, "hPa");
    addReading(readings, "pressure_sea", toSeaLevel(hPa, tempC, ALTITUDE_M), "hPa");

    if (readings.size() == 0) {
        Serial.println("[sensor] nothing finite to send");
        return false;
    }

    String body;
    serializeJson(doc, body);

    WiFiClient  client;
    HTTPClient  http;
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/api/ingest",
             COLLECTOR_HOST, (int)COLLECTOR_PORT);

    if (!http.begin(client, url)) {
        Serial.println("[post] http.begin failed");
        return false;
    }
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ingest-Token", INGEST_TOKEN);
    if (strlen(COLLECTOR_BASIC_USER) > 0) {
        http.setAuthorization(COLLECTOR_BASIC_USER, COLLECTOR_BASIC_PASS);
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
    Serial.printf("\n\nESP32_Logger node \"%s\" -> %s:%d\n",
                  NODE_ID, COLLECTOR_HOST, (int)COLLECTOR_PORT);

    s_sensorReady = initSensor();
    ensureWifi();
}

void loop() {
    const uint32_t now = millis();

    // Unsigned subtraction, so the ~49-day millis() wrap is a non-event.
    if (s_postedOnce && (now - s_lastPost) < POST_INTERVAL_MS) {
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
