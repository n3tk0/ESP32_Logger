// ============================================================================
// src/web/FirstRunHandler.cpp — see FirstRunHandler.h for contract.
// ============================================================================
#include "FirstRunHandler.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "../core/Globals.h"
#include "../core/BoardProfiles.h"
#include "../core/Config.h"
#include "../managers/ConfigManager.h"
#include "../utils/JsonResponse.h"

namespace {

// ---------------------------------------------------------------------------
// GET /api/board-profiles
// ---------------------------------------------------------------------------
// Returns the registered profile catalogue so the wizard can render the
// board picker + show the right pin restrictions per option.
void handleGetBoardProfiles(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["profiles"].to<JsonArray>();

    const BoardProfile* list[8];
    uint8_t n = listProfiles(list, 8);
    for (uint8_t i = 0; i < n; i++) {
        const BoardProfile* p = list[i];
        JsonObject o    = arr.add<JsonObject>();
        o["id"]         = p->shortId;
        o["name"]       = p->name;
        o["maxGpio"]    = p->maxGpio;

        auto fill = [&](const char* key, const uint8_t* src) {
            JsonArray a = o[key].to<JsonArray>();
            for (uint8_t k = 0; k < MAX_RESTRICTED_PINS && src[k] != PIN_UNSET; k++) {
                a.add(src[k]);
            }
        };
        fill("strapPins",    p->strapPins);
        fill("usbPins",      p->usbPins);
        fill("flashPins",    p->flashPins);
        fill("reservedPins", p->reservedPins);
    }

    JsonObject active = doc["active"].to<JsonObject>();
    active["id"]      = g_boardProfile ? g_boardProfile->shortId : "";
    active["setupRequired"] = g_setupRequired;

    sendJsonResponse(req, doc);
}

// ---------------------------------------------------------------------------
// POST /api/firstrun
// ---------------------------------------------------------------------------
// Body:
//   {
//     "profile": "xiao_c3",
//     "mode":    "legacy" | "continuous" | "hybrid",
//     "pins": {                                  // optional, legacy/hybrid only
//       "wifiTrigger": 4,
//       "wakeupFF":    3,
//       "wakeupPF":    10,
//       "flowSensor":  20,
//       "rtcCE":       21,
//       "rtcIO":       1,
//       "rtcSCLK":     0
//     }
//   }
//
// Validation rules per pin (when profile != "custom"):
//   - in [0, profile->maxGpio]
//   - not in strap / USB / flash / reserved lists
//   - not duplicate of any other pin in the same body
//
// On success: persists /board_profile.txt, updates config.hardware if pins
// supplied, saveConfig(), clears g_setupRequired, schedules a reboot.

void handlePostFirstRun(AsyncWebServerRequest* req,
                        uint8_t* data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }

    const char* shortId = doc["profile"] | "";
    const BoardProfile* profile = getProfileByShortId(shortId);
    if (!profile) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"unknown profile\"}");
        return;
    }

    const char* modeStr = doc["mode"] | "";
    bool legacyPipeline = (strcmp(modeStr, "legacy") == 0 ||
                          strcmp(modeStr, "hybrid") == 0);

    // ---- Pin validation (only required for legacy/hybrid; continuous
    // mode configures sensor pins individually in /settings/sensors) -----
    struct PinAssignment { const char* key; uint8_t value; };
    PinAssignment pins[] = {
        { "wifiTrigger", (uint8_t)(doc["pins"]["wifiTrigger"] | (int)PIN_UNSET) },
        { "wakeupFF",    (uint8_t)(doc["pins"]["wakeupFF"]    | (int)PIN_UNSET) },
        { "wakeupPF",    (uint8_t)(doc["pins"]["wakeupPF"]    | (int)PIN_UNSET) },
        { "flowSensor",  (uint8_t)(doc["pins"]["flowSensor"]  | (int)PIN_UNSET) },
        { "rtcCE",       (uint8_t)(doc["pins"]["rtcCE"]       | (int)PIN_UNSET) },
        { "rtcIO",       (uint8_t)(doc["pins"]["rtcIO"]       | (int)PIN_UNSET) },
        { "rtcSCLK",     (uint8_t)(doc["pins"]["rtcSCLK"]     | (int)PIN_UNSET) },
    };
    constexpr int N_PINS = sizeof(pins) / sizeof(pins[0]);

    if (legacyPipeline) {
        // Per-pin: reject if assigned (!= PIN_UNSET) AND fails board rules.
        // Unassigned (PIN_UNSET) is allowed here — user may legitimately
        // leave optional features (e.g. RTC, WiFi-trigger) unconfigured.
        // The runtime guard in commit 8 will refuse to attach an unassigned
        // pin at use time, surfacing a clear error.
        for (int i = 0; i < N_PINS; i++) {
            if (pins[i].value == PIN_UNSET) continue;
            if (!isPinAllowed(profile, pins[i].value, PIN_PURPOSE_GENERIC)) {
                char body[160];
                snprintf(body, sizeof(body),
                         "{\"ok\":false,\"error\":\"pin %s = GPIO%u rejected: %s\"}",
                         pins[i].key, pins[i].value,
                         pinRejectReason(profile, pins[i].value));
                req->send(400, "application/json", body);
                return;
            }
        }
        // Duplicate detection. Two pins assigned to the same GPIO is always
        // an error, even on BOARD_CUSTOM.
        for (int i = 0; i < N_PINS; i++) {
            if (pins[i].value == PIN_UNSET) continue;
            for (int j = i + 1; j < N_PINS; j++) {
                if (pins[j].value == pins[i].value) {
                    char body[160];
                    snprintf(body, sizeof(body),
                             "{\"ok\":false,\"error\":\"duplicate pin: %s and %s both = GPIO%u\"}",
                             pins[i].key, pins[j].key, pins[i].value);
                    req->send(400, "application/json", body);
                    return;
                }
            }
        }
    }

    // ---- Commit ------------------------------------------------------------
    if (!BoardProfiles::save(profile)) {
        req->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"failed to write /board_profile.txt\"}");
        return;
    }
    g_boardProfile = profile;

    if (legacyPipeline) {
        config.hardware.pinWifiTrigger = pins[0].value;
        config.hardware.pinWakeupFF    = pins[1].value;
        config.hardware.pinWakeupPF    = pins[2].value;
        config.hardware.pinFlowSensor  = pins[3].value;
        config.hardware.pinRtcCE       = pins[4].value;
        config.hardware.pinRtcIO       = pins[5].value;
        config.hardware.pinRtcSCLK     = pins[6].value;
        saveConfig();
    }

    g_setupRequired = false;

    // Schedule reboot — same mechanism used by /restart elsewhere.
    extern bool          shouldRestart;
    extern unsigned long restartTimer;
    shouldRestart  = true;
    restartTimer   = millis();

    req->send(200, "application/json",
              "{\"ok\":true,\"message\":\"setup saved, rebooting\"}");
}

}  // namespace

// ---------------------------------------------------------------------------
void registerFirstRunRoutes() {
    extern AsyncWebServer server;

    // /firstrun → /www/firstrun.html. serveStatic maps /firstrun.html
    // directly; this alias lets the FirstRunGate redirect target work
    // without forcing users to type the extension.
    server.on("/firstrun", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(LittleFS, "/www/firstrun.html", "text/html");
    });

    server.on("/api/board-profiles", HTTP_GET, handleGetBoardProfiles);

    server.on("/api/firstrun", HTTP_POST,
        // Request callback (no-op; body handled below)
        [](AsyncWebServerRequest* r) { /* see onBody */ },
        // Upload callback (none)
        nullptr,
        // Body callback — single-shot, no chunking accepted
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (index != 0 || len != total) {
                r->send(413, "application/json",
                        "{\"ok\":false,\"error\":\"body too large\"}");
                return;
            }
            handlePostFirstRun(r, data, len);
        });
}
