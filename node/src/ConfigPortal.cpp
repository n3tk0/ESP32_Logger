#include "ConfigPortal.h"

// The NODE_SENSOR_* selection decides which pin fields the form shows.
#include "node_config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>

// The sync web server is the right shape here: the portal is a handful of
// requests from one phone, it runs only while nothing else is happening, and
// blocking inside a handler costs nothing when there is no sensor loop to
// stall.
static ESP8266WebServer s_http(80);
static DNSServer        s_dns;
static NodeSettings*    s_target = nullptr;
static bool             s_saved  = false;

/// True while the portal is being served on the STA interface — the home LAN —
/// rather than on the node's own access point.
///
/// THE DIFFERENCE IS WHO CAN REACH IT. To open the AP portal you have to be
/// associated with the node's access point, standing next to it, during a
/// window it opens only after repeated WiFi failures. The background portal is
/// on the LAN, for the node's whole uptime, reachable by anything on the
/// network. The same page cannot be safe in both places, so it is not the same
/// page: secrets are rendered on one and blanked on the other.
static bool s_background = false;

/// Handlers are registered once for the life of the process.
///
/// ESP8266WebServer::on() appends to a list and never deduplicates, and
/// portalStartBackground() is called on every transition back to connected —
/// so a node whose router reboots nightly grew three dead handler entries a
/// night, walked on every request, until it ran out of heap.
static bool s_routesBound = false;

/// Basic auth on the background portal, using the credentials the node already
/// holds for the collector.
///
/// Reusing them is not elegant, but the alternative was worse in both
/// directions: a new setting nobody would fill in, or — as shipped — a form on
/// the LAN that anyone could POST to, repointing the node at their own
/// collector and restarting it. The AP portal is not gated, because reaching it
/// already requires the AP's own password.
static bool backgroundAuthOk() {
    if (!s_background) return true;
    const NodeSettings& s = *s_target;
    if (s_http.authenticate(s.basicUser, s.basicPass)) return true;
    s_http.requestAuthentication();
    return false;
}

bool portalButtonHeld() {
    pinMode(PORTAL_TRIGGER_PIN, INPUT_PULLUP);
    // One read can catch a floating pin mid-transition; require the button to
    // still be down after a debounce interval.
    if (digitalRead(PORTAL_TRIGGER_PIN) != LOW) return false;
    delay(50);
    return digitalRead(PORTAL_TRIGGER_PIN) == LOW;
}

// ---------------------------------------------------------------------------
// A value going into an HTML attribute has to be escaped. A WiFi passphrase
// containing a double quote is legal and not rare, and without this it would
// break out of the value attribute and silently truncate the field — the kind
// of bug that looks like "the portal corrupts my password".
// ---------------------------------------------------------------------------
static String esc(const char* raw) {
    String out;
    for (const char* p = raw; *p; p++) {
        switch (*p) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *p;          break;
        }
    }
    return out;
}

static void row(String& p, const char* id, const char* label,
                const String& value, const char* type, const char* hint) {
    p += F("<label for=\""); p += id; p += F("\">"); p += label;
    p += F("</label><input id=\""); p += id;
    p += F("\" name=\""); p += id;
    p += F("\" type=\""); p += type;
    p += F("\" value=\""); p += value;
    p += F("\">");
    if (hint && *hint) { p += F("<p class=\"h\">"); p += hint; p += F("</p>"); }
}

static void handleRoot() {
    if (!backgroundAuthOk()) return;
    const NodeSettings& s = *s_target;

    String p;
    p.reserve(4000);
    p += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Sensor node setup</title><style>"
           "body{font-family:system-ui,-apple-system,sans-serif;max-width:32rem;"
           "margin:0 auto;padding:1rem;background:#f6f7f9;color:#111}"
           "h1{font-size:1.3rem}h2{font-size:.8rem;text-transform:uppercase;"
           "letter-spacing:.08em;color:#666;margin:1.5rem 0 .5rem;"
           "border-bottom:1px solid #ddd;padding-bottom:.3rem}"
           "label{display:block;font-weight:600;font-size:.9rem;margin-top:.8rem}"
           "input{width:100%;padding:.5rem;font-size:1rem;border:1px solid #bbb;"
           "border-radius:6px;box-sizing:border-box;background:#fff}"
           ".h{font-size:.8rem;color:#666;margin:.25rem 0 0}"
           "button{margin-top:1.5rem;width:100%;padding:.7rem;font-size:1rem;"
           "font-weight:600;border:0;border-radius:6px;background:#275673;"
           "color:#fff}</style></head><body><h1>Sensor node setup</h1>"
           "<form method=\"POST\" action=\"/save\">");

    p += F("<h2>WiFi</h2>");
    row(p, "ssid", "Network name", esc(s.ssid), "text", "");
    // Rendered ONLY on the access-point portal, where seeing it is a
    // convenience for whoever is standing in front of the node and had to know
    // the AP password to get here. On the LAN portal the field is blank and an
    // empty submission keeps what is stored: that page is reachable by
    // everything on the network for as long as the node is up, and putting the
    // home WiFi passphrase in its HTML source would hand it to all of them.
    if (s_background) {
        row(p, "pass", "Password", String(), "password",
            s.pass[0] ? "Leave empty to keep the saved password."
                      : "Required.");
    } else {
        row(p, "pass", "Password", esc(s.pass), "password",
            "<input type=\"checkbox\" style=\"width:auto;display:inline-block;margin-right:5px;vertical-align:middle\" "
            "onclick=\"document.getElementById('pass').type = this.checked ? 'text' : 'password'\">"
            "<span style=\"vertical-align:middle\">Show password</span>");
    }

    p += F("<h2>Collector</h2>");
    row(p, "host", "IP address", esc(s.host), "text",
        "The ESP32-C3. Give it a DHCP reservation on your router.");
    row(p, "port", "Port", String(s.port), "number", "");
    // Same rule as the passphrase: a shared secret, shown where only someone
    // already on the node's own AP can see it, blank on the LAN.
    if (s_background) {
        row(p, "token", "Ingest token", String(), "password",
            s.token[0] ? "Leave empty to keep the saved token."
                       : "Must match INGEST_TOKEN on the collector.");
    } else {
        row(p, "token", "Ingest token", esc(s.token), "text",
            "Must match INGEST_TOKEN on the collector.");
    }
    row(p, "buser", "Basic auth user", esc(s.basicUser), "text",
        "Only if the collector was built with WEB_BASIC_AUTH_ENABLED.");
    row(p, "bpass", "Basic auth password", String(), "password",
        s.basicPass[0] ? "Leave empty to keep the saved password." : "");

    // Only sensors actually compiled into this build get pin fields. Offering
    // a 1-Wire pin on a build with no DS18B20 driver would be a control that
    // does nothing, which is worse than no control at all.
#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688) || \
    defined(NODE_SENSOR_BH1750) || defined(NODE_SENSOR_DS18B20) || \
    defined(NODE_SENSOR_PULSE)  || defined(NODE_SENSOR_SDS011)
    p += F("<h2>Sensor pins</h2>");
#endif

#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688) || \
    defined(NODE_SENSOR_BH1750)
    row(p, "sda", "I2C SDA (GPIO)", String(s.i2cSda), "number",
        "Shared by every I2C sensor. NodeMCU V3: D2 = GPIO4, D1 = GPIO5.");
    row(p, "scl", "I2C SCL (GPIO)", String(s.i2cScl), "number", "");
#endif
#if defined(NODE_SENSOR_DS18B20)
    row(p, "ow", "1-Wire data (GPIO)", String(s.oneWirePin), "number",
        "Needs a 4.7k pull-up to 3V3. Avoid GPIO0 and GPIO2 — both are boot "
        "straps.");
#endif
#if defined(NODE_SENSOR_PULSE)
    row(p, "pulse", "Pulse input (GPIO)", String(s.pulsePin), "number",
        PULSE_MODE_RAIN ? "Rain gauge reed switch, to GND."
                        : "Hall flow sensor signal line.");
#endif
#if defined(NODE_SENSOR_SDS011)
    row(p, "sdsrx", "SDS011 RX (GPIO)", String(s.sdsRx), "number",
        "Wire the sensor's TXD here. GPIO16 will not work — it cannot raise "
        "interrupts.");
    row(p, "sdstx", "SDS011 TX (GPIO)", String(s.sdsTx), "number", "");
#endif

    p += F("<h2>This node</h2>");
    row(p, "nodeid", "Node id", esc(s.nodeId), "text",
        "Must match the \"node\" field of the collector's remote sensor.");
    row(p, "interval", "Post interval (seconds)",
        String(s.intervalMs / 1000UL), "number", "");
    row(p, "alt", "Altitude (m)", String(s.altitudeM, 1), "number",
        "Used to report sea-level pressure. 0 sends station pressure only.");

    p += F("<button type=\"submit\">Save and restart</button>"
           "</form></body></html>");

    s_http.send(200, "text/html", p);
}

static void handleSave() {
    // Checked HERE too, not only on the form. A POST does not have to come from
    // a page this device served, and this is the handler that rewrites the
    // node's collector address and then restarts it.
    if (!backgroundAuthOk()) return;

    // Build into a COPY. Writing straight into the caller's settings would
    // leave them corrupted on the validation and write-failure paths below:
    // portalRun() promises to return with `s` unmodified, and a node that
    // fell back to retrying its saved network would instead be running
    // against a half-applied config from RAM until the next power cycle,
    // even though flash still holds a good one.
    NodeSettings s = *s_target;

    auto text = [&](const char* field, char* dst, size_t len) {
        if (!s_http.hasArg(field)) return;
        strncpy(dst, s_http.arg(field).c_str(), len - 1);
        dst[len - 1] = '\0';
    };
    // An empty password field means "keep the saved one", so it must not
    // overwrite. Clearing a password therefore needs an erase, which is the
    // right trade: accidentally blanking WiFi credentials from a phone
    // keyboard is far likelier than deliberately wanting an open network.
    auto secret = [&](const char* field, char* dst, size_t len) {
        if (!s_http.hasArg(field)) return;
        const String v = s_http.arg(field);
        if (v.length() == 0) return;
        strncpy(dst, v.c_str(), len - 1);
        dst[len - 1] = '\0';
    };

    text  ("ssid",   s.ssid,      sizeof(s.ssid));
    // secret(), not text(). The LAN form serves these fields blank, so a plain
    // copy would wipe the passphrase and the token on every save from it. On
    // the AP form the fields arrive pre-filled, so "empty means keep" only
    // costs the ability to blank them there — which an erase already covers,
    // and which is far rarer than clearing a field by accident on a phone.
    secret("pass",   s.pass,      sizeof(s.pass));
    text  ("host",   s.host,      sizeof(s.host));
    secret("token",  s.token,     sizeof(s.token));
    text  ("buser",  s.basicUser, sizeof(s.basicUser));
    secret("bpass",  s.basicPass, sizeof(s.basicPass));
    text  ("nodeid", s.nodeId,    sizeof(s.nodeId));

    if (s_http.hasArg("port")) {
        const long v = s_http.arg("port").toInt();
        s.port = (v > 0 && v <= 65535) ? (uint16_t)v : 80;
    }
    if (s_http.hasArg("interval")) {
        long secs = s_http.arg("interval").toInt();
        // A node posting every second would hit the collector's rate limiter
        // and fill its ingest table with churn; an hour is past the point
        // where a "live" outdoor reading means anything.
        if (secs < 10)   secs = 10;
        if (secs > 3600) secs = 3600;
        s.intervalMs = (uint32_t)secs * 1000UL;
    }
    if (s_http.hasArg("alt")) {
        s.altitudeM = s_http.arg("alt").toFloat();
    }

    // GPIO numbers. The ESP8266 has 0-16; anything outside that is a typo, and
    // silently accepting it would produce a sensor that never initialises with
    // no clue why. Out-of-range keeps the previous value.
    auto pin = [&](const char* field, uint8_t& dst) {
        if (!s_http.hasArg(field)) return;
        const long v = s_http.arg(field).toInt();
        if (v >= 0 && v <= 16) dst = (uint8_t)v;
    };
    pin("sda",   s.i2cSda);
    pin("scl",   s.i2cScl);
    pin("ow",    s.oneWirePin);
    pin("pulse", s.pulsePin);
    pin("sdsrx", s.sdsRx);
    pin("sdstx", s.sdsTx);

    if (!s.isComplete()) {
        s_http.send(400, "text/html",
                    F("<!DOCTYPE html><meta charset=\"utf-8\">"
                      "<p style=\"font-family:sans-serif\">A network name and a "
                      "collector address are both required. "
                      "<a href=\"/\">Back</a></p>"));
        return;
    }

    if (!settingsSave(s)) {
        s_http.send(500, "text/html",
                    F("<!DOCTYPE html><meta charset=\"utf-8\">"
                      "<p style=\"font-family:sans-serif\">Could not write to "
                      "flash. <a href=\"/\">Back</a></p>"));
        return;
    }

    // Only now that it is safely on flash does the caller's copy change.
    *s_target = s;

    s_http.send(200, "text/html",
                F("<!DOCTYPE html><meta charset=\"utf-8\">"
                  "<p style=\"font-family:sans-serif\">Saved. Restarting &mdash; "
                  "this network will disappear.</p>"));
    s_saved = true;
}

bool portalRun(NodeSettings& s, uint32_t timeoutMs) {
    s_target     = &s;
    s_saved      = false;
    s_background = false;   // the AP portal: secrets shown, no auth gate

    char ap[32];
    // The MAC suffix keeps two nodes in one house apart on the air.
    snprintf(ap, sizeof(ap), "esp-node-%04X",
             (uint16_t)(ESP.getChipId() & 0xFFFF));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap, PORTAL_AP_PASS);
    delay(100);

    const IPAddress ip = WiFi.softAPIP();
    Serial.printf("[portal] \"%s\" up at http://%s  (pass: %s)\n",
                  ap, ip.toString().c_str(), PORTAL_AP_PASS);
    if (timeoutMs) Serial.printf("[portal] closing in %lu s\n",
                                 (unsigned long)(timeoutMs / 1000));

    // Answer every DNS query with our own address so phones show the
    // "sign in to network" prompt instead of leaving the user to discover
    // 192.168.4.1 on their own.
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    if (!s_routesBound) {
        s_http.on("/", handleRoot);
        s_http.on("/save", HTTP_POST, handleSave);
        // Captive-portal probes hit vendor-specific URLs; sending them the form
        // is what makes the prompt open straight into it.
        s_http.onNotFound(handleRoot);
        s_routesBound = true;
    }
    s_http.begin();

    uint32_t start = millis();
    bool     hadClient = false;
    while (!s_saved) {
        s_dns.processNextRequest();
        s_http.handleClient();

        // THE CLOCK STOPS WHILE SOMEBODY IS CONNECTED.
        //
        // The timeout exists so a node does not sit in AP mode forever after a
        // 3 am router reboot nobody witnessed. A station associated to the
        // softAP is the opposite of that: it is direct evidence a human is
        // standing there, mid-configuration.
        //
        // Without this the window closed under them. Joining the AP, waiting
        // out the phone's "no internet, stay connected?" prompt, and typing an
        // SSID and a passphrase is comfortably more than the five minutes the
        // timer allowed from BOOT — so the page would half-load, or not load
        // at all, and the AP would vanish. From the outside that is
        // indistinguishable from a portal that never worked.
        const bool client = WiFi.softAPgetStationNum() > 0;
        if (client) {
            start = millis();          // hold the window open
            hadClient = true;
        } else if (hadClient) {
            // They left without saving. Give the timeout a fresh start rather
            // than resuming a stale count, so a reconnect gets a full window.
            hadClient = false;
            start = millis();
        }

        if (timeoutMs && !client && (millis() - start) > timeoutMs) {
            Serial.println("[portal] timed out, retrying the saved network");
            break;
        }
        delay(5);   // feeds the softAP task; yield() alone starves it here
    }

    if (s_saved) {
        // Let the browser collect the confirmation page before the AP drops.
        const uint32_t until = millis() + 1500;
        while ((int32_t)(millis() - until) < 0) {
            s_http.handleClient();
            delay(5);
        }
    }

    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    return s_saved;
}

bool portalStartBackground(NodeSettings& s) {
    // FAILS CLOSED. Without credentials there is nothing between this form and
    // everything on the network, and the form rewrites the node's collector
    // address and restarts it. Refusing is a feature the user has to turn on by
    // setting a password, which is the right way round; serving it anyway would
    // be a remote-configuration interface nobody asked to expose.
    if (s.basicUser[0] == '\0' || s.basicPass[0] == '\0') {
        Serial.println(F("[portal] background server NOT started: set a basic-auth "
                         "user and password in the setup portal to enable "
                         "configuration over the LAN"));
        return false;
    }

    s_target     = &s;
    s_saved      = false;
    s_background = true;

    // Once for the life of the process. on() appends without deduplicating, so
    // re-registering on every reconnect leaked three entries per router outage.
    if (!s_routesBound) {
        s_http.on("/", handleRoot);
        s_http.on("/save", HTTP_POST, handleSave);
        s_http.onNotFound(handleRoot);
        s_routesBound = true;
    }
    s_http.begin();
    Serial.printf("[portal] background configuration server on http://%s/ "
                  "(password protected)\n", WiFi.localIP().toString().c_str());
    return true;
}

void portalHandleClient() {
    s_http.handleClient();
    if (s_saved) {
        // Let the browser collect the confirmation page
        const uint32_t until = millis() + 1500;
        while ((int32_t)(millis() - until) < 0) {
            s_http.handleClient();
            delay(5);
        }
        Serial.println("[portal] Settings saved via background server, restarting");
        ESP.restart();
    }
}
