#include "Portal.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>

#include "ConfigStore.h"
#include "FwTrial.h"
#include "node_config.h"
#include "src/nodecfg/FwImage.h"
#include "src/nodecfg/NodeConfigJson.h"
#include "src/nodecfg/NodePortalPage.h"

using namespace nodecfg;

static WebServer   s_http(80);
static DNSServer   s_dns;
static NodeConfig* s_cfg  = nullptr;
static NodeLink    s_link;
static float     (*s_battV)() = nullptr;
static uint32_t    s_restartAt = 0;     ///< millis() to restart at, 0 = not scheduled

// ---------------------------------------------------------------------------
// The button
// ---------------------------------------------------------------------------

bool portalButtonHeld() {
    pinMode(NODE_PORTAL_PIN, INPUT_PULLUP);
    // One read can catch a floating pin mid-transition; require the button to
    // still be down after a debounce interval.
    if (digitalRead(NODE_PORTAL_PIN) != LOW) return false;
    delay(50);
    return digitalRead(NODE_PORTAL_PIN) == LOW;
}

/// WHY NOT "HOLD BOOT THROUGH RESET". On the ESP32-C3 the BOOT button is
/// GPIO9, a strapping pin: held low while the chip comes out of reset, it
/// selects the ROM's USB download mode, and this firmware never runs. So the
/// gesture is: press RESET (or plug the node in), THEN press and hold BOOT —
/// and on a boot like that the firmware watches the button for
/// NODE_PORTAL_WINDOW_MS before doing anything else. Deep-sleep wakes and
/// software restarts do not wait (a wake that waited two seconds would cost
/// more than the wake itself); they only take a single look, so a button held
/// down across a wake still works.
bool portalRequested(bool coldStart) {
    if (portalButtonHeld()) return true;
    if (!coldStart || NODE_PORTAL_WINDOW_MS == 0) return false;
    Serial.printf("[portal] hold BOOT now for the setup page (%u ms)\n",
                  (unsigned)NODE_PORTAL_WINDOW_MS);
    const uint32_t t0 = millis();
    while (millis() - t0 < NODE_PORTAL_WINDOW_MS) {
        if (portalButtonHeld()) return true;
        delay(20);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void sendJson(int code, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    s_http.sendHeader("Cache-Control", "no-store");
    s_http.send(code, "application/json", out);
}

static void handleRoot() {
    s_http.sendHeader("Content-Encoding", "gzip");
    s_http.sendHeader("Cache-Control", "no-store");
    s_http.send_P(200, "text/html; charset=utf-8", (const char*)NODE_PORTAL_GZ,
                  NODE_PORTAL_GZ_LEN);
}

static void handleConfigGet() {
    JsonDocument doc;
    // No secrets (an ESP-NOW node has only the key), and the key itself only
    // as "lmk": "" + "lmk_set" — principle 5.
    encodeConfig(*s_cfg, doc["config"].to<JsonObject>(), NCJ_LMK);
    encodeCaps(Transport::EspNow, Hw::Esp32c3, doc["caps"].to<JsonObject>());
    sendJson(200, doc);
}

static void refuse(int code, const char* field, const char* reason) {
    JsonDocument doc;
    doc["ok"]     = false;
    doc["field"]  = field;
    doc["reason"] = reason;
    sendJson(code, doc);
}

static void handleConfigPost() {
    if (s_restartAt) return refuse(409, "", "already saved; restarting");

    JsonDocument in;
    if (deserializeJson(in, s_http.arg("plain")) != DeserializationError::Ok)
        return refuse(400, "", "the body is not JSON");

    // Decode on top of what is running (a partial document is an edit), with
    // the key allowed — this page is the one place it may be typed (§0.6).
    static NodeConfig next;              // ~1 KB: off the web task's stack
    next = *s_cfg;
    Issue err;
    if (!decodeConfig(in.as<JsonVariantConst>(), next, NCJ_DEC_LMK, &err))
        return refuse(400, err.field, err.reason);

    Validation v;
    if (!validate(next, v)) {
        JsonDocument out;
        encodeValidation(v, out.to<JsonObject>());
        return sendJson(400, out);
    }

    // Local edits win (principle 4): the node reports this document on its
    // next contact and the collector adopts it as the new desired rev.
    next.local = true;
    if (!cfgStoreSave(next) || !cfgStoreSaveKey(next.lmk))
        return refuse(500, "", "could not write the settings to flash");
    *s_cfg = next;

    JsonDocument out;
    encodeValidation(v, out.to<JsonObject>());   // {"ok":true} + any warnings
    sendJson(200, out);
    Serial.println("[portal] saved; restarting");
    s_restartAt = millis() + 500;
    if (s_restartAt == 0) s_restartAt = 1;
}

/// The scan runs in the background (WiFi.scanNetworks(true)) and the page
/// polls: a C3 scan is a couple of seconds, and holding the one web-server
/// thread for it would stall the phone's own connection to the AP.
static void handleScan() {
    JsonDocument doc;
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        doc["state"] = "running";
    } else if (n >= 0) {
        doc["state"] = "done";
        JsonArray nets = doc["nets"].to<JsonArray>();
        int listed = 0;
        for (int i = 0; i < n && listed < 20; i++) {
            const String ssid = WiFi.SSID(i);
            if (!ssid.length()) continue;            // hidden networks are not offered
            JsonObject o = nets.add<JsonObject>();
            o["ssid"] = ssid;
            o["rssi"] = WiFi.RSSI(i);
            o["ch"]   = WiFi.channel(i);
            o["enc"]  = (int)WiFi.encryptionType(i);   // WIFI_AUTH_OPEN = 0 (§6)
            listed++;
        }
        WiFi.scanDelete();                             // the next GET scans afresh
    } else {
        // WIFI_SCAN_FAILED is also "no scan has been started": start one.
        WiFi.scanNetworks(true, false);
        doc["state"] = "running";
    }
    sendJson(200, doc);
}

static void handleStatus() {
    JsonDocument doc;
    doc["uptime_s"]  = millis() / 1000;
    doc["rssi"]      = nullptr;              // not associated to anything
    doc["last_ok_s"] = nullptr;
    doc["ip"]        = WiFi.softAPIP().toString();
    doc["mac"]       = WiFi.macAddress();
    doc["rev"]       = s_cfg->rev;
    doc["local"]     = s_cfg->local;
    // ESP-NOW is off while the page runs, so the collector's reachability is
    // not known; what is known is the stored link (§6).
    doc["collector"] = "unknown";
    doc["paired"]    = s_link.nodeId != 0;
    doc["node_id"]   = s_link.nodeId;
    doc["ch"]        = s_link.channel;
    const float v = s_battV ? s_battV() : 0.0f;
    if (v > 0.0f) doc["batt_v"] = serialized(String(v, 3));
    else          doc["batt_v"] = nullptr;
    sendJson(200, doc);
}

// ---------------------------------------------------------------------------
// POST /update — a firmware from the page (docs/NODE_OTA.md §5)
// ---------------------------------------------------------------------------
// Streamed into the next OTA slot by the core's Update library while the
// same checks run over it that a download from the collector gets: the head
// (an ESP32-C3 app image), the NODEFW1 marker (an ESP-NOW node's, not the
// collector's or the WiFi node's), and the slot's size. Whatever fails, the
// update is abort()ed: Update holds the image's first 16 bytes back until
// end(), so a refused or half-written slot is never bootable and the running
// firmware is untouched. end() itself switches only after the image's own
// SHA-256 checks out (esp_ota_set_boot_partition).
//
// The version on the page is the config's `fw` (/api/config), set from
// NODE_FW_VERSION at every load (ConfigStore.cpp) — the same field the WiFi
// node's page reads.

static struct {
    bool     started;        ///< a file part arrived
    bool     failed;
    bool     headChecked;
    int      code;
    const char* error;       ///< §5's error word
    char     detail[48];
    uint32_t got;
    uint32_t slot;           ///< largest image that fits
    uint8_t  head[nodefw::HEAD_NEED];
    nodefw::MarkerScan scan;
} s_up;

static void upFail(int code, const char* error, const char* detail) {
    if (s_up.failed) return;             // the first reason is the one to report
    s_up.failed = true;
    s_up.code   = code;
    s_up.error  = error;
    copyStr(s_up.detail, sizeof(s_up.detail), detail ? detail : "");
    if (Update.isRunning()) Update.abort();
    Serial.printf("[portal] update refused: %s %s\n", error, s_up.detail);
}

static void handleUpdateUpload() {
    HTTPUpload& u = s_http.upload();
    switch (u.status) {
        case UPLOAD_FILE_START: {
            memset(&s_up, 0, sizeof(s_up));
            nodefw::scanBegin(s_up.scan);
            s_up.started = true;
            const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
            const uint32_t kindMax = nodefw::maxSize(nodefw::KIND_ESPNOW_C3);
            s_up.slot = p ? (p->size < kindMax ? p->size : kindMax) : 0;
            if (!p || !Update.begin(UPDATE_SIZE_UNKNOWN))
                upFail(500, "write_failed", p ? Update.errorString() : "no update partition");
            return;
        }
        case UPLOAD_FILE_WRITE: {
            if (s_up.failed || !s_up.started) return;
            if ((uint64_t)s_up.got + u.currentSize > s_up.slot)
                return upFail(400, "too_big", "");
            if (s_up.got < sizeof(s_up.head)) {
                size_t n = sizeof(s_up.head) - s_up.got;
                if (n > u.currentSize) n = u.currentSize;
                memcpy(s_up.head + s_up.got, u.buf, n);
            }
            s_up.got += u.currentSize;
            // Judged the moment the head is complete — normally in the first
            // chunk (the server hands them over ~1.4 KB at a time) — so a
            // wrong file is refused before most of it is written.
            if (!s_up.headChecked && s_up.got >= sizeof(s_up.head)) {
                s_up.headChecked = true;
                if (!nodefw::headOk(nodefw::KIND_ESPNOW_C3, s_up.head, sizeof(s_up.head)))
                    return upFail(400, "not_node_image", "");
            }
            nodefw::scanFeed(s_up.scan, u.buf, u.currentSize);
            if (Update.write(u.buf, u.currentSize) != u.currentSize)
                upFail(500, "write_failed", Update.errorString());
            return;
        }
        case UPLOAD_FILE_END:
            if (s_up.failed || !s_up.started) return;
            if (!s_up.headChecked || nodefw::scanKind(s_up.scan) != nodefw::KIND_ESPNOW_C3)
                return upFail(400, "not_node_image", "");
            if (!Update.end(true)) upFail(500, "write_failed", Update.errorString());
            return;
        case UPLOAD_FILE_ABORTED:
            upFail(500, "write_failed", "upload interrupted");
            return;
    }
}

static void handleUpdateDone() {
    if (!s_up.started) upFail(400, "not_node_image", "no file");
    JsonDocument doc;
    if (s_up.failed) {
        doc["ok"]    = false;
        doc["error"] = s_up.error;
        if (s_up.detail[0]) doc["detail"] = (const char*)s_up.detail;
        sendJson(s_up.code, doc);
        return;
    }
    doc["ok"] = true;
    sendJson(200, doc);
    // A remote update's trial would go back to the slot this one just
    // replaced; the person who uploaded it is the check now (§5).
    fwTrialClear();
    Serial.printf("[portal] firmware %s written (%lu bytes); restarting\n", s_up.scan.ver,
                  (unsigned long)s_up.got);
    s_restartAt = millis() + 1000;
    if (s_restartAt == 0) s_restartAt = 1;
}

static void handleNotFound() {
    // Captive-portal probes (generate_204, hotspot-detect.html, …) and any
    // stray path get the page: phones then show "sign in to network".
    if (s_http.uri().startsWith("/api/")) {
        s_http.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    handleRoot();
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

void portalRun(NodeConfig& cfg, const NodeLink& link, float (*battV)()) {
    s_cfg   = &cfg;
    s_link  = link;
    s_battV = battV;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap[20];
    snprintf(ap, sizeof(ap), "esp-node-%02X%02X", mac[4], mac[5]);

    // AP + STA from the start: the station half never associates, it is only
    // there so /api/scan can scan without switching modes under the phone.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    WiFi.softAP(ap, PORTAL_AP_PASS);
    delay(100);

    const IPAddress ip = WiFi.softAPIP();
    Serial.printf("[portal] \"%s\" up at http://%s  (pass: %s), closing after %lu s idle\n",
                  ap, ip.toString().c_str(), PORTAL_AP_PASS,
                  (unsigned long)(PORTAL_TIMEOUT_MS / 1000));

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    s_http.on("/", HTTP_GET, handleRoot);
    s_http.on("/api/config", HTTP_GET, handleConfigGet);
    s_http.on("/api/config", HTTP_POST, handleConfigPost);
    s_http.on("/api/scan", HTTP_GET, handleScan);
    s_http.on("/api/status", HTTP_GET, handleStatus);
    s_http.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    s_http.onNotFound(handleNotFound);
    s_http.begin();

    uint32_t start = millis();
    for (;;) {
        s_dns.processNextRequest();
        s_http.handleClient();

        if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) break;

        // THE CLOCK STOPS WHILE SOMEBODY IS CONNECTED, and restarts from zero
        // when they leave: a station on the AP is a person mid-setup, and
        // joining, dismissing the phone's "no internet" prompt and walking
        // through six steps can take longer than five minutes. The timeout is
        // for the node nobody came to — on a battery, an AP left up is a cell
        // emptied in a day.
        if (WiFi.softAPgetStationNum() > 0) start = millis();
        if (millis() - start > PORTAL_TIMEOUT_MS) {
            Serial.println("[portal] nobody came; restarting");
            break;
        }
        delay(2);
    }

    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.flush();
    ESP.restart();
    for (;;) {}
}
