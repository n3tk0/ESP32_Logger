// ============================================================================
// src/core/BoardProfiles.cpp — see BoardProfiles.h for contract.
// ============================================================================
#include "BoardProfiles.h"

#include <string.h>
#include <Arduino.h>
#include <LittleFS.h>
#include "../utils/AtomicWrite.h"
#include "../utils/Utils.h"             // validatePin (USB CDC integration)
#include "../pipeline/DataPipeline.h"   // fsMutex
#include "Globals.h"                    // activeFS, fsAvailable, g_boardProfile

// Per-sensor "allow restricted pins" override (see BoardProfiles.h). Set by
// SensorManager around each sensor's init(); single-threaded init context.
bool g_pinAllowUnsafe = false;


// Pin-restriction tables. Sentinel-terminated with PIN_UNSET. Verified
// against the Espressif ESP32-C3 / ESP32-S3 technical reference manuals
// and the Seeed XIAO ESP32-C3 datasheet (rev 2024-02).
//
// Strap pins — sampled at reset; pulling them to wrong levels at power-on
// changes boot mode (download mode, JTAG enable, etc).
//
//   ESP32-C3:  GPIO 2, 8, 9
//   ESP32-S3:  GPIO 0, 3, 45, 46
//
// Flash bus — internal SPI flash on packages with embedded flash. The
// XIAO C3 and SuperMini C3 both ship with embedded 4 MB flash, so these
// GPIOs are not exposed but listed here as a defensive belt-and-braces:
//
//   ESP32-C3 (4 MB embedded): GPIO 11-17
//   ESP32-S3 (8 MB embedded): GPIO 26-32 (octal flash) or 26-37 (octal PSRAM)
//
// USB CDC — when USB-Serial-JTAG is enabled (default on Arduino C3/S3
// builds), these pins are not usable for GPIO. These are now handled
// dynamically by validatePin() at runtime, allowing users to toggle
// USB CDC ON/OFF via the deploy tool. See UsbCdcModule.
//
//   ESP32-C3: GPIO 18 (D-), 19 (D+)  [checked by validatePin() at runtime]
//   ESP32-S3: GPIO 19 (D-), 20 (D+)  [checked by validatePin() at runtime]
//
// Reserved — UART0 console (Serial.printf debug output). These pins work
// as GPIO if the user accepts losing serial debug output, so we list them
// in `reservedPins` (still blocked by default but easier to override).
//
//   ESP32-C3: GPIO 20 (U0RXD), 21 (U0TXD)
//   ESP32-S3: GPIO 43 (U0TXD), 44 (U0RXD)

namespace {

// Note: positional initialisers (not designated `.field = …`) so the
// toolchain's C++ standard mode doesn't matter — arduino-esp32 defaults
// to gnu++17 / gnu++20 across versions and we want this file to compile
// on every supported one. Field order matches BoardProfile in the header.

// --- Seeed XIAO ESP32-C3 -----------------------------------------------------
// USB CDC (pins 18/19) is now handled by validatePin() at runtime, not statically.
// This allows USB CDC to be toggled ON/OFF via deploy tool without changing profiles.
constexpr BoardProfile XIAO_C3 = {
    BOARD_XIAO_C3,
    "Seeed XIAO ESP32-C3",
    "xiao_c3",
    21,
    { 2, 8, 9, PIN_UNSET },
    { PIN_UNSET },  // USB CDC handled by validatePin() dynamically
    { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    { 20, 21, PIN_UNSET },
    { PIN_UNSET },  // absentPins: generic/legacy profile — see header
};

// --- Generic "ESP32-C3 SuperMini" --------------------------------------------
// Same C3 silicon; some clones break out GPIO 8 to a status LED, others
// don't. Same restriction set as XIAO C3 for safety.
// USB CDC (pins 18/19) is now handled by validatePin() at runtime.
constexpr BoardProfile SUPERMINI_C3 = {
    BOARD_SUPERMINI_C3,
    "ESP32-C3 SuperMini",
    "supermini_c3",
    21,
    { 2, 8, 9, PIN_UNSET },
    { PIN_UNSET },  // USB CDC handled by validatePin() dynamically
    { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    { 20, 21, PIN_UNSET },
    { PIN_UNSET },  // absentPins: generic/legacy profile — see header
};

// --- Bare ESP32-C3 module ----------------------------------------------------
// No board-specific quirks; same chip constraints.
// USB CDC (pins 18/19) is now handled by validatePin() at runtime,
// allowing it to be toggled ON/OFF via deploy tool.
constexpr BoardProfile GENERIC_C3 = {
    BOARD_GENERIC_C3,
    "Generic ESP32-C3",
    "generic_c3",
    21,
    { 2, 8, 9, PIN_UNSET },
    { PIN_UNSET },  // USB CDC handled by validatePin() dynamically
    { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    { 20, 21, PIN_UNSET },
    { PIN_UNSET },  // absentPins: generic/legacy profile — see header
};

// --- Bare ESP32-S3 module ----------------------------------------------------
// Wider GPIO range. Strap pins per S3 TRM. Flash range varies by package;
// use the most defensive set (octal flash + octal PSRAM = 26-37).
// USB CDC (pins 19/20) is now handled by validatePin() at runtime.
// GPIO pins are indexed 0-47 (48 total), so maxGpio is 47
constexpr BoardProfile GENERIC_S3 = {
    BOARD_GENERIC_S3,
    "Generic ESP32-S3",
    "generic_s3",
    47,
    { 0, 3, 45, 46, PIN_UNSET },
    { PIN_UNSET },  // USB CDC handled by validatePin() dynamically
    { 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, PIN_UNSET },
    { 43, 44, PIN_UNSET },
    { PIN_UNSET },  // absentPins: generic/legacy profile — see header
};

// --- Seeed XIAO ESP32-S3 -----------------------------------------------------
// Same S3 silicon as GENERIC_S3, but the thumb-sized carrier routes only
// eleven GPIOs to the castellated header:
//
//   D0=1  D1=2  D2=3  D3=4  D4=5  D5=6  D6=43  D7=44  D8=7  D9=8  D10=9
//
// maxGpio is therefore 44, which also disposes of the 45/46 straps and 47/48.
// Everything the silicon has between the header pins is listed in absentPins
// so the wizard stops offering GPIOs that do not leave the module — the
// failure mode otherwise is a sensor that never answers and never logs why.
//
// GPIO 3 (D2) IS broken out but stays in strapPins: it is the S3's JTAG-source
// strap, same as on the generic profile. GPIO 19/20 (USB D-/D+) go to the USB-C
// connector and are checked dynamically by validatePin(), not listed here.
// Flash + octal PSRAM claim 26-37; the module has both, so the full range
// applies rather than the flash-only subset.
constexpr BoardProfile XIAO_S3 = {
    BOARD_XIAO_S3,
    "Seeed XIAO ESP32-S3",
    "xiao_s3",
    44,
    { 0, 3, PIN_UNSET },
    { PIN_UNSET },  // USB CDC handled by validatePin() dynamically
    { 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, PIN_UNSET },
    { 43, 44, PIN_UNSET },
    // Not routed to the header. 10-11 and several of 12-18 reach the camera/SD
    // B2B connector on the XIAO S3 Sense, which is why these stay overridable
    // via allow_unsafe_pins rather than being hard-blocked.
    { 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, PIN_UNSET },
};

// --- Custom (user accepts responsibility) ------------------------------------
// Empty restriction lists. isPinAllowed() short-circuits on this id.
constexpr BoardProfile CUSTOM = {
    BOARD_CUSTOM,
    "Custom \xE2\x80\x94 full responsibility",  // UTF-8 em dash
    "custom",
    48,                       // S3 upper bound; permissive
    { PIN_UNSET },
    { PIN_UNSET },
    { PIN_UNSET },
    { PIN_UNSET },
    { PIN_UNSET },  // absentPins: generic/legacy profile — see header
};

const BoardProfile* const ALL_PROFILES[] = {
    &XIAO_C3,
    &SUPERMINI_C3,
    &GENERIC_C3,
    &GENERIC_S3,
    &XIAO_S3,
    &CUSTOM,
};
constexpr uint8_t ALL_PROFILES_COUNT = sizeof(ALL_PROFILES) / sizeof(ALL_PROFILES[0]);
static_assert(ALL_PROFILES_COUNT <= MAX_PROFILES,
              "Profile list exceeds MAX_PROFILES — bump MAX_PROFILES in BoardProfiles.h");

// A list initialised with exactly MAX_RESTRICTED_PINS entries compiles fine
// and reads correctly — inList() stops at the cap — but has nowhere left to
// put the terminator, so the next pin appended to it walks off the end.
// Catch that at build time rather than paying flash for permanent slack.
// Written as single-return recursion, not a loop: arduino-esp32 2.x compiles
// this project as -std=gnu++11, where a constexpr function body may only be a
// return statement. (The note above about gnu++17/20 describes newer cores.)
constexpr bool terminatedFrom(const uint8_t (&list)[MAX_RESTRICTED_PINS], uint8_t i) {
    return i >= MAX_RESTRICTED_PINS ? false
         : (list[i] == PIN_UNSET    ? true
                                    : terminatedFrom(list, (uint8_t)(i + 1)));
}
constexpr bool terminated(const uint8_t (&list)[MAX_RESTRICTED_PINS]) {
    return terminatedFrom(list, 0);
}
constexpr bool allTerminated(const BoardProfile& p) {
    return terminated(p.strapPins)    && terminated(p.usbPins)
        && terminated(p.flashPins)    && terminated(p.reservedPins)
        && terminated(p.absentPins);
}
#define ASSERT_TERMINATED(profile)                                            \
    static_assert(allTerminated(profile),                                     \
                  #profile " has a pin list with no room for PIN_UNSET — "     \
                  "shorten it or bump MAX_RESTRICTED_PINS in BoardProfiles.h")
ASSERT_TERMINATED(XIAO_C3);
ASSERT_TERMINATED(SUPERMINI_C3);
ASSERT_TERMINATED(GENERIC_C3);
ASSERT_TERMINATED(GENERIC_S3);
ASSERT_TERMINATED(XIAO_S3);
ASSERT_TERMINATED(CUSTOM);
#undef ASSERT_TERMINATED

bool inList(const uint8_t* list, uint8_t pin) {
    for (uint8_t i = 0; i < MAX_RESTRICTED_PINS && list[i] != PIN_UNSET; i++) {
        if (list[i] == pin) return true;
    }
    return false;
}

}  // namespace

// ----------------------------------------------------------------------------
const BoardProfile* getProfileByShortId(const char* shortId) {
    if (shortId == nullptr || *shortId == '\0') return nullptr;
    for (uint8_t i = 0; i < ALL_PROFILES_COUNT; i++) {
        if (strcmp(ALL_PROFILES[i]->shortId, shortId) == 0) return ALL_PROFILES[i];
    }
    return nullptr;
}

uint8_t listProfiles(const BoardProfile** out, uint8_t outCap) {
    uint8_t n = (outCap < ALL_PROFILES_COUNT) ? outCap : ALL_PROFILES_COUNT;
    for (uint8_t i = 0; i < n; i++) out[i] = ALL_PROFILES[i];
    return n;
}

// ----------------------------------------------------------------------------
bool isPinAllowed(const BoardProfile* profile, uint8_t pin, PinPurpose /*purpose*/) {
    if (profile == nullptr)        return false;   // no board selected
    if (pin == PIN_UNSET)          return false;   // explicit "unassigned"
    if (pin > profile->maxGpio)    return false;
    if (profile->id == BOARD_CUSTOM) return true;  // user owns the risk
    if (inList(profile->strapPins,    pin)) return false;
    if (inList(profile->usbPins,      pin)) return false;
    if (inList(profile->flashPins,    pin)) return false;
    if (inList(profile->reservedPins, pin)) return false;
    if (inList(profile->absentPins,   pin)) return false;
    return true;
}

bool validateAttachPin(int pin, const char* sensorId, const char* fieldName) {
    if (pin < 0 || pin == (int)PIN_UNSET) {
        Serial.printf("[%s.%s] init refused: pin not assigned\n",
                      sensorId ? sensorId : "?",
                      fieldName ? fieldName : "pin");
        return false;
    }
    if (pin > 255) {
        Serial.printf("[%s.%s] init refused: GPIO%d out of range\n",
                      sensorId ? sensorId : "?",
                      fieldName ? fieldName : "pin", pin);
        return false;
    }

    // ── Centralized pin validation (Pillar 4.2/4.11) ────────────────────
    // Includes USB CDC runtime conflict detection
    String usage = String(sensorId) + "." + String(fieldName);
    if (!validatePin(pin, usage)) {
        // validatePin() already logged the conflict details
        Serial.printf("[%s.%s] init refused: GPIO%d validation failed\n",
                      sensorId ? sensorId : "?",
                      fieldName ? fieldName : "pin", pin);
        return false;
    }

    // ── Board profile static validation ──────────────────────────────────
    if (!isPinAllowed(g_boardProfile, (uint8_t)pin, PIN_PURPOSE_GENERIC)) {
        const char* reason = pinRejectReason(g_boardProfile, (uint8_t)pin);
        // allow_unsafe_pins (per-sensor) can override a strapping- or
        // reserved-pin refusal — those are usable with correct wiring (e.g. an
        // I2C pull-up holds a C3 strap pin HIGH, its safe boot level). Flash-bus
        // and out-of-range pins never work, so they stay hard-blocked.
        bool canOverride = g_pinAllowUnsafe
                        && g_boardProfile != nullptr
                        && g_boardProfile->id != BOARD_CUSTOM
                        && (uint8_t)pin <= g_boardProfile->maxGpio
                        && !inList(g_boardProfile->flashPins, (uint8_t)pin);
        if (!canOverride) {
            Serial.printf("[%s.%s] init refused: GPIO%d = %s%s\n",
                          sensorId ? sensorId : "?",
                          fieldName ? fieldName : "pin", pin, reason,
                          g_pinAllowUnsafe ? " (cannot be overridden)" : "");
            return false;
        }
        Serial.printf("[%s.%s] WARNING: GPIO%d = %s — allowed via allow_unsafe_pins; "
                      "ensure proper pull-ups, the device may fail to boot if this pin "
                      "is held LOW at reset\n",
                      sensorId ? sensorId : "?",
                      fieldName ? fieldName : "pin", pin, reason);
    }
    return true;
}

const char* pinRejectReason(const BoardProfile* profile, uint8_t pin) {
    if (profile == nullptr)        return "no board profile selected";
    if (pin == PIN_UNSET)          return "pin not assigned";
    if (pin > profile->maxGpio)    return "GPIO out of range for board";
    if (profile->id == BOARD_CUSTOM) return "ok";
    if (inList(profile->strapPins,    pin)) return "bootstrap pin (boot mode risk)";
    // Note: USB CDC pins are checked dynamically by validatePin(), not here
    if (inList(profile->flashPins,    pin)) return "SPI flash bus pin";
    if (inList(profile->reservedPins, pin)) return "reserved (UART0 console)";
    if (inList(profile->absentPins,   pin)) return "not broken out on this board";
    return "ok";
}

// ============================================================================
// Persistence — /board_profile.txt
// ============================================================================
namespace BoardProfiles {

constexpr const char* kPath        = "/board_profile.txt";
constexpr size_t      kMaxFileSize = 256;   // tiny key=value file

const BoardProfile* load() {
    if (!fsAvailable || !activeFS) return nullptr;
    if (!activeFS->exists(kPath))  return nullptr;

    File f = activeFS->open(kPath, FILE_READ);
    if (!f) return nullptr;
    if (f.size() == 0 || f.size() > kMaxFileSize) { f.close(); return nullptr; }

    char buf[kMaxFileSize + 1];
    size_t n = f.read(reinterpret_cast<uint8_t*>(buf), kMaxFileSize);
    f.close();
    buf[n] = '\0';

    // Find profile=<shortId>
    char shortId[32] = {0};
    char* p = strstr(buf, "profile=");
    if (!p) return nullptr;
    p += 8;
    size_t i = 0;
    while (*p && *p != '\n' && *p != '\r' && i < sizeof(shortId) - 1) {
        shortId[i++] = *p++;
    }
    shortId[i] = '\0';

    return getProfileByShortId(shortId);
}

bool save(const BoardProfile* profile) {
    if (!profile)                return false;
    if (!fsAvailable || !activeFS) return false;

    char body[128];
    int n = snprintf(body, sizeof(body),
                     "profile=%s\nversion=1\n", profile->shortId);
    if (n <= 0 || n >= (int)sizeof(body)) return false;

    return atomicWrite(*activeFS, kPath, [&](File& dst) -> bool {
        return dst.write(reinterpret_cast<const uint8_t*>(body), (size_t)n)
               == (size_t)n;
    }, fsMutex);
}

}  // namespace BoardProfiles
