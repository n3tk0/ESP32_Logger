#include "CsrfToken.h"
#include <ESPAsyncWebServer.h>
#include <esp_system.h>     // esp_random()

namespace {
    char s_token[33] = "";   // 32 hex chars + NUL

    void ensureToken() {
        if (s_token[0]) return;
        // 16 bytes (128 bits) of entropy from the hardware RNG, hex-encoded.
        for (int i = 0; i < 4; i++) {
            uint32_t r = esp_random();
            for (int j = 0; j < 4; j++) {
                uint8_t b = (uint8_t)(r >> (j * 8));
                static const char hex[] = "0123456789abcdef";
                s_token[(i * 8) + (j * 2)]     = hex[b >> 4];
                s_token[(i * 8) + (j * 2) + 1] = hex[b & 0x0f];
            }
        }
        s_token[32] = '\0';
    }

    // Constant-time compare so a malicious caller can't time-side-channel
    // partial matches.  Always walks the full 32-byte length without an
    // early-exit branch — the caller already pre-validates length and the
    // stored token is a 32-char hex string with no embedded NULs (gemini
    // review PR #52).
    bool secureEq(const char* a, const char* b) {
        if (!a || !b) return false;
        unsigned diff = 0;
        for (int i = 0; i < 32; i++) {
            diff |= (unsigned)(a[i] ^ b[i]);
        }
        return diff == 0;
    }
}

const char* CsrfToken::get() {
    ensureToken();
    return s_token;
}

bool CsrfToken::require(AsyncWebServerRequest* req) {
    ensureToken();
    // ESPAsyncWebServer drops custom request headers unless individual
    // handlers opt in via addInterestingHeader; rather than touch every
    // server.on() site we accept the token as a form/query param.  Common
    // CSRF pattern in many web apps; no security difference for the
    // double-submit-cookie threat model since the SPA already injects
    // the token into every mutating call.
    String supplied;
    if (req->hasParam("csrf", true)) {        // POST body
        supplied = req->getParam("csrf", true)->value();
    } else if (req->hasParam("csrf")) {       // query string
        supplied = req->getParam("csrf")->value();
    }

    if (supplied.length() == 32 && secureEq(supplied.c_str(), s_token)) {
        return true;
    }

    // Reject — same shape as the rate-limit 429 so client error handling
    // can be uniform.
    AsyncWebServerResponse* resp = req->beginResponse(
        403, "application/json",
        "{\"ok\":false,\"error\":\"csrf\"}");
    resp->addHeader("Connection", "close");
    req->send(resp);
    return false;
}
