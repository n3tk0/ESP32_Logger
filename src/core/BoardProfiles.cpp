// ============================================================================
// src/core/BoardProfiles.cpp — see BoardProfiles.h for contract.
// ============================================================================
#include "BoardProfiles.h"

#include <string.h>

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
// builds), these pins are not usable for GPIO:
//
//   ESP32-C3: GPIO 18 (D-), 19 (D+)
//   ESP32-S3: GPIO 19 (D-), 20 (D+)
//
// Reserved — UART0 console (Serial.printf debug output). These pins work
// as GPIO if the user accepts losing serial debug output, so we list them
// in `reservedPins` (still blocked by default but easier to override).
//
//   ESP32-C3: GPIO 20 (U0RXD), 21 (U0TXD)
//   ESP32-S3: GPIO 43 (U0TXD), 44 (U0RXD)

namespace {

// --- Seeed XIAO ESP32-C3 -----------------------------------------------------
const BoardProfile XIAO_C3 = {
    .id           = BOARD_XIAO_C3,
    .name         = "Seeed XIAO ESP32-C3",
    .shortId      = "xiao_c3",
    .maxGpio      = 21,
    .strapPins    = { 2, 8, 9, PIN_UNSET },
    .usbPins      = { 18, 19, PIN_UNSET },
    .flashPins    = { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    .reservedPins = { 20, 21, PIN_UNSET },
};

// --- Generic "ESP32-C3 SuperMini" --------------------------------------------
// Same C3 silicon; some clones break out GPIO 8 to a status LED, others
// don't. Same restriction set as XIAO C3 for safety.
const BoardProfile SUPERMINI_C3 = {
    .id           = BOARD_SUPERMINI_C3,
    .name         = "ESP32-C3 SuperMini",
    .shortId      = "supermini_c3",
    .maxGpio      = 21,
    .strapPins    = { 2, 8, 9, PIN_UNSET },
    .usbPins      = { 18, 19, PIN_UNSET },
    .flashPins    = { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    .reservedPins = { 20, 21, PIN_UNSET },
};

// --- Bare ESP32-C3 module ----------------------------------------------------
// No board-specific quirks; same chip constraints. USB CDC may or may not
// be enabled depending on build config — keep 18/19 restricted to be safe.
const BoardProfile GENERIC_C3 = {
    .id           = BOARD_GENERIC_C3,
    .name         = "Generic ESP32-C3",
    .shortId      = "generic_c3",
    .maxGpio      = 21,
    .strapPins    = { 2, 8, 9, PIN_UNSET },
    .usbPins      = { 18, 19, PIN_UNSET },
    .flashPins    = { 11, 12, 13, 14, 15, 16, 17, PIN_UNSET },
    .reservedPins = { 20, 21, PIN_UNSET },
};

// --- Bare ESP32-S3 module ----------------------------------------------------
// Wider GPIO range. Strap pins per S3 TRM. Flash range varies by package;
// use the most defensive set (octal flash + octal PSRAM = 26-37).
const BoardProfile GENERIC_S3 = {
    .id           = BOARD_GENERIC_S3,
    .name         = "Generic ESP32-S3",
    .shortId      = "generic_s3",
    .maxGpio      = 48,
    .strapPins    = { 0, 3, 45, 46, PIN_UNSET },
    .usbPins      = { 19, 20, PIN_UNSET },
    .flashPins    = { 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, PIN_UNSET },
    .reservedPins = { 43, 44, PIN_UNSET },
};

// --- Custom (user accepts responsibility) ------------------------------------
// Empty restriction lists. isPinAllowed() short-circuits on this id.
const BoardProfile CUSTOM = {
    .id           = BOARD_CUSTOM,
    .name         = "Custom — full responsibility",
    .shortId      = "custom",
    .maxGpio      = 48,                         // S3 upper bound; permissive
    .strapPins    = { PIN_UNSET },
    .usbPins      = { PIN_UNSET },
    .flashPins    = { PIN_UNSET },
    .reservedPins = { PIN_UNSET },
};

const BoardProfile* const ALL_PROFILES[] = {
    &XIAO_C3,
    &SUPERMINI_C3,
    &GENERIC_C3,
    &GENERIC_S3,
    &CUSTOM,
};
constexpr uint8_t ALL_PROFILES_COUNT = sizeof(ALL_PROFILES) / sizeof(ALL_PROFILES[0]);

bool inList(const uint8_t* list, uint8_t pin) {
    for (uint8_t i = 0; i < MAX_RESTRICTED_PINS && list[i] != PIN_UNSET; i++) {
        if (list[i] == pin) return true;
    }
    return false;
}

}  // namespace

// ----------------------------------------------------------------------------
const BoardProfile* getProfileById(BoardProfileId id) {
    if (id == BOARD_NONE) return nullptr;
    for (uint8_t i = 0; i < ALL_PROFILES_COUNT; i++) {
        if (ALL_PROFILES[i]->id == id) return ALL_PROFILES[i];
    }
    return nullptr;
}

const BoardProfile* getProfileByShortId(const char* shortId) {
    if (shortId == nullptr || *shortId == '\0') return nullptr;
    for (uint8_t i = 0; i < ALL_PROFILES_COUNT; i++) {
        if (strcmp(ALL_PROFILES[i]->shortId, shortId) == 0) return ALL_PROFILES[i];
    }
    return nullptr;
}

uint8_t listProfilesCount() {
    return ALL_PROFILES_COUNT;
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
    return true;
}

const char* pinRejectReason(const BoardProfile* profile, uint8_t pin) {
    if (profile == nullptr)        return "no board profile selected";
    if (pin == PIN_UNSET)          return "pin not assigned";
    if (pin > profile->maxGpio)    return "GPIO out of range for board";
    if (profile->id == BOARD_CUSTOM) return "ok";
    if (inList(profile->strapPins,    pin)) return "bootstrap pin (boot mode risk)";
    if (inList(profile->usbPins,      pin)) return "USB CDC pin (D+/D-)";
    if (inList(profile->flashPins,    pin)) return "SPI flash bus pin";
    if (inList(profile->reservedPins, pin)) return "reserved (UART0 console)";
    return "ok";
}
