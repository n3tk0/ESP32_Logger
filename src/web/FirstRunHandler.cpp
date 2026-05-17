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
#include "../utils/AtomicWrite.h"
#include "../utils/JsonResponse.h"
#include "../pipeline/DataPipeline.h"   // fsMutex

namespace {

// ---------------------------------------------------------------------------
// Updates the top-level "mode" field in /platform_config.json without
// disturbing the rest of the file. Read-modify-write under atomicWrite so
// a crash mid-update never leaves a truncated config.
//
// Mirrors how _detectPlatformMode() in ESP_Logger.ino consumes the file —
// any value other than "continuous" or "hybrid" is interpreted as legacy.
bool persistPlatformMode(const char* mode) {
    if (!fsAvailable || !activeFS) return false;
    constexpr const char* PATH        = "/platform_config.json";
    constexpr size_t      MAX_SIZE    = 16 * 1024;

    JsonDocument doc;
    if (activeFS->exists(PATH)) {
        File f = activeFS->open(PATH, FILE_READ);
        if (!f) return false;
        if (f.size() > MAX_SIZE) { f.close(); return false; }
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;
    }
    doc["mode"] = mode;

    return atomicWrite(*activeFS, PATH, [&](File& dst) -> bool {
        size_t want    = measureJson(doc);
        size_t written = serializeJson(doc, dst);
        return written == want;
    }, fsMutex);
}

// ---------------------------------------------------------------------------
// GET /api/board-profiles
// ---------------------------------------------------------------------------
// Returns the registered profile catalogue so the wizard can render the
// board picker + show the right pin restrictions per option.
void handleGetBoardProfiles(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["profiles"].to<JsonArray>();

    const BoardProfile* list[MAX_PROFILES];
    uint8_t n = listProfiles(list, MAX_PROFILES);
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

    // Pin layout:
    //   - Always-required-in-all-modes: WiFi-trigger button + the two wake-
    //     up buttons. These run the device's physical UI in EVERY mode —
    //     including continuous — so the wizard must collect them. (Fix for
    //     Gemini HIGH review on PR #87 — leaving them PIN_UNSET in
    //     continuous mode broke buttons + AP-mode trigger.)
    //   - Legacy/hybrid only: flow sensor + DS1302 RTC trio.
    //
    // Per-pin policy: PIN_UNSET allowed (user may legitimately skip an
    // optional feature); strap/USB/flash/reserved pins rejected with a
    // specific reason; duplicates rejected regardless of profile.
    struct PinAssignment { const char* key; uint8_t value; bool legacyOnly; };
    PinAssignment pins[] = {
        { "wifiTrigger", (uint8_t)(doc["pins"]["wifiTrigger"] | (int)PIN_UNSET), false },
        { "wakeupFF",    (uint8_t)(doc["pins"]["wakeupFF"]    | (int)PIN_UNSET), false },
        { "wakeupPF",    (uint8_t)(doc["pins"]["wakeupPF"]    | (int)PIN_UNSET), false },
        { "flowSensor",  (uint8_t)(doc["pins"]["flowSensor"]  | (int)PIN_UNSET), true  },
        { "rtcCE",       (uint8_t)(doc["pins"]["rtcCE"]       | (int)PIN_UNSET), true  },
        { "rtcIO",       (uint8_t)(doc["pins"]["rtcIO"]       | (int)PIN_UNSET), true  },
        { "rtcSCLK",     (uint8_t)(doc["pins"]["rtcSCLK"]     | (int)PIN_UNSET), true  },
    };
    constexpr int N_PINS = sizeof(pins) / sizeof(pins[0]);

    // Per-pin validation — applied to every supplied pin regardless of mode.
    // Continuous-only deployments may legitimately leave the legacy-only pins
    // at PIN_UNSET; runtime guard catches a use attempt.
    auto inScope = [&](const PinAssignment& p) -> bool {
        return legacyPipeline || !p.legacyOnly;
    };
    for (int i = 0; i < N_PINS; i++) {
        if (!inScope(pins[i])) continue;
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
    for (int i = 0; i < N_PINS; i++) {
        if (!inScope(pins[i]) || pins[i].value == PIN_UNSET) continue;
        for (int j = i + 1; j < N_PINS; j++) {
            if (!inScope(pins[j]) || pins[j].value == PIN_UNSET) continue;
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

    // ---- Commit ------------------------------------------------------------
    if (!BoardProfiles::save(profile)) {
        req->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"failed to write /board_profile.txt\"}");
        return;
    }
    g_boardProfile = profile;

    // Always persist the universal-pin trio (WiFi-trigger + buttons).
    // Legacy-only pins are persisted only when the mode owns them.
    config.hardware.pinWifiTrigger = pins[0].value;
    config.hardware.pinWakeupFF    = pins[1].value;
    config.hardware.pinWakeupPF    = pins[2].value;
    if (legacyPipeline) {
        config.hardware.pinFlowSensor  = pins[3].value;
        config.hardware.pinRtcCE       = pins[4].value;
        config.hardware.pinRtcIO       = pins[5].value;
        config.hardware.pinRtcSCLK     = pins[6].value;
    }
    saveConfig();

    // Persist the operating mode pick into /platform_config.json so that
    // _detectPlatformMode() on the next boot reflects the user's choice.
    // Without this the device reverts to "legacy" regardless of the wizard
    // (Gemini HIGH on PR #87).
    if (!persistPlatformMode(modeStr)) {
        req->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"failed to write /platform_config.json\"}");
        return;
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
        // Body callback — accumulates chunks via _tempObject so chunked
        // delivery from low-MTU clients doesn't get rejected. Payload is
        // bounded at 4 KB; oversized bodies abort early.
        [](AsyncWebServerRequest* r, uint8_t* data, size_t len,
           size_t index, size_t total) {
            constexpr size_t MAX_BODY = 4 * 1024;
            if (total > MAX_BODY) {
                r->send(413, "application/json",
                        "{\"ok\":false,\"error\":\"body too large\"}");
                return;
            }
            if (index == 0) {
                r->_tempObject = new (std::nothrow) String();
                if (!r->_tempObject) {
                    r->send(500, "application/json",
                            "{\"ok\":false,\"error\":\"out_of_memory\"}");
                    return;
                }
                static_cast<String*>(r->_tempObject)->reserve(total);
            }
            String* buf = static_cast<String*>(r->_tempObject);
            if (!buf) return;   // earlier OOM
            buf->concat(reinterpret_cast<const char*>(data), len);
            if (index + len >= total) {
                handlePostFirstRun(r,
                                   (uint8_t*)buf->c_str(),
                                   buf->length());
                delete buf;
                r->_tempObject = nullptr;
            }
        });
}
