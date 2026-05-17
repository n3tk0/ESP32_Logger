// ============================================================================
// src/core/BoardProfiles.h
//
// Board awareness for pin assignment validation.
//
// Every pin-entry surface (first-run wizard, sensor config UI,
// SensorManager::init, POST /api/modules/:id) consults the active board
// profile via isPinAllowed() before accepting a GPIO number. The profile
// encodes what's unsafe on a given board: bootstrap pins, USB CDC pins,
// SPI flash pins, and other reserved GPIOs.
//
// A device's selected profile is persisted in /board_profile.txt (read at
// boot by BoardProfiles::load). Until the user selects one via the
// first-run wizard, the active profile is BOARD_NONE and pin validation
// rejects every assignment, forcing the wizard to run.
//
// Closes AUDIT 5.3, 5.7, 5.8, 5.9, 23.1, 31.2, 31.3 by removing every
// unsafe hardware default; pulls the strap-pin runtime guard work in
// from R16 since the wizard validation surface needs it co-located.
// ============================================================================
#pragma once

#include <stdint.h>

// Universal "no pin assigned" sentinel for uint8_t-typed GPIO fields.
// uint8_t can't hold -1, so we use 0xFF — well outside the GPIO range of
// any supported ESP32 family (C3 = 0-21, S3 = 0-48). Matches IsrPin's
// detached-state convention.
constexpr uint8_t PIN_UNSET = 0xFF;

// Logical roles a pin can be assigned to. Used by isPinAllowed() to apply
// purpose-specific extra checks (e.g. an analog-only role can't be served
// by a digital-only pin on some boards; UART pins shouldn't be repurposed
// as general I/O if console is in use).
enum PinPurpose : uint8_t {
    PIN_PURPOSE_GENERIC    = 0,
    PIN_PURPOSE_DIGITAL_IN = 1,
    PIN_PURPOSE_DIGITAL_OUT= 2,
    PIN_PURPOSE_ANALOG_IN  = 3,
    PIN_PURPOSE_I2C_SDA    = 4,
    PIN_PURPOSE_I2C_SCL    = 5,
    PIN_PURPOSE_UART_RX    = 6,
    PIN_PURPOSE_UART_TX    = 7,
    PIN_PURPOSE_PULSE_IN   = 8,
};

enum BoardProfileId : uint8_t {
    BOARD_NONE          = 0,   // no profile selected → wizard must run
    BOARD_XIAO_C3       = 1,   // Seeed XIAO ESP32-C3
    BOARD_SUPERMINI_C3  = 2,   // generic "ESP32-C3 SuperMini" clone
    BOARD_GENERIC_C3    = 3,   // bare ESP32-C3 module
    BOARD_GENERIC_S3    = 4,   // bare ESP32-S3 module
    BOARD_CUSTOM        = 99,  // user accepts full responsibility; no validation
};

// Pin-restriction sets per profile. Variable-length arrays in C++ at
// namespace scope mean we use fixed caps and sentinel-terminate.
// MAX_RESTRICTED_PINS is sized for the worst case (S3 has many).
constexpr uint8_t MAX_RESTRICTED_PINS = 16;

struct BoardProfile {
    BoardProfileId  id;
    const char*     name;          // human-readable, shown in wizard
    const char*     shortId;       // wire format used in /board_profile.txt
    uint8_t         maxGpio;       // highest valid GPIO number on this part
    // Sentinel-terminated (PIN_UNSET marks end). Lists are kept short and
    // sorted ascending for easy human review.
    uint8_t         strapPins   [MAX_RESTRICTED_PINS];  // bootstrap / boot-mode
    uint8_t         usbPins     [MAX_RESTRICTED_PINS];  // CDC USB D+/D-
    uint8_t         flashPins   [MAX_RESTRICTED_PINS];  // SPI flash bus
    uint8_t         reservedPins[MAX_RESTRICTED_PINS];  // UART0 console, etc.
};

// --- Profile lookups ---------------------------------------------------------

/// Returns the profile descriptor for `id`, or nullptr if id is BOARD_NONE
/// or unknown. BOARD_CUSTOM returns a valid pointer with empty restriction
/// lists — isPinAllowed() always returns true for that profile.
const BoardProfile* getProfileById(BoardProfileId id);

/// Returns the profile matching `shortId` (e.g. "xiao_c3"), or nullptr.
const BoardProfile* getProfileByShortId(const char* shortId);

/// Returns the count of registered profiles (including BOARD_CUSTOM).
/// `out[]` must be sized for at least listProfilesCount() entries.
uint8_t listProfiles(const BoardProfile** out, uint8_t outCap);
uint8_t listProfilesCount();

// --- Validation --------------------------------------------------------------

/// Returns true if `pin` is safe to assign on the active board profile.
/// `purpose` lets the profile apply role-specific checks (e.g. an analog
/// role on a digital-only pin). Currently every supported board can serve
/// every digital role on every non-restricted pin, so `purpose` is
/// accepted for future use and parity with the JS-side helper.
///
/// PIN_UNSET (0xFF) returns false — it's the explicit "no pin chosen"
/// state and never represents a valid assignment.
///
/// If `profile == nullptr` (no board selected): always returns false.
/// If `profile->id == BOARD_CUSTOM`: returns true for any pin in 0..maxGpio.
bool isPinAllowed(const BoardProfile* profile,
                  uint8_t              pin,
                  PinPurpose           purpose = PIN_PURPOSE_GENERIC);

/// Returns a short human-readable reason ("strap pin", "USB D+/D-", etc.)
/// for why `pin` is not allowed on `profile`. Returns "ok" if the pin is
/// allowed, or "no board profile" if profile is nullptr. The pointer is
/// to a static string; do not free.
const char* pinRejectReason(const BoardProfile* profile, uint8_t pin);
