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
    BOARD_XIAO_S3       = 5,   // Seeed XIAO ESP32-S3
    BOARD_CUSTOM        = 99,  // user accepts full responsibility; no validation
};

// Pin-restriction sets per profile. Variable-length arrays in C++ at
// namespace scope mean we use fixed caps and sentinel-terminate.
// MAX_RESTRICTED_PINS is sized for the worst case (S3 has many).
//
// An over-long initialiser is a compile error ("too many initializers"), but a
// list filled to exactly MAX_RESTRICTED_PINS with no room for the sentinel is
// NOT — inList() stops at the cap and the list still reads correctly today,
// while any later append silently walks past the end. BoardProfiles.cpp
// static_asserts that every list is sentinel-terminated instead of buying
// slack here: the cap is per-list per-profile, so raising it by 8 costs ~240
// bytes of flash on targets that are already at 93 %.
//
// Longest list in use: xiao_s3's absentPins, 15 entries + sentinel.
constexpr uint8_t MAX_RESTRICTED_PINS = 16;

// Upper bound for the number of registered board profiles. Callers use
// this to size on-stack arrays for listProfiles(). BoardProfiles.cpp has
// a static_assert against ALL_PROFILES_COUNT to catch a silent overflow
// if the profile list grows past this cap (Gemini PR #87 review).
constexpr uint8_t MAX_PROFILES = 16;

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
    // GPIOs the silicon has but this particular board does not route to a
    // header pin. Electrically fine — there is simply nothing to solder to,
    // so offering them in the wizard produces a sensor that never responds
    // and never errors. Overridable (like strap/reserved) because module
    // pads and B2B connectors do exist: the XIAO S3 Sense carries several of
    // its "absent" GPIOs to the camera/SD connector.
    //
    // Only populated for profiles named after a specific board. A generic
    // module profile cannot know the carrier, and existing profiles are left
    // empty on purpose — filling them in would newly refuse pins that
    // deployed devices are already configured with.
    uint8_t         absentPins  [MAX_RESTRICTED_PINS];
};

// --- Profile lookups ---------------------------------------------------------

/// Returns the profile matching `shortId` (e.g. "xiao_c3"), or nullptr.
const BoardProfile* getProfileByShortId(const char* shortId);

/// Fills `out[]` with up to `outCap` registered profile pointers (including
/// BOARD_CUSTOM) and returns the number written.
uint8_t listProfiles(const BoardProfile** out, uint8_t outCap);

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

/// Runtime guard called by sensor plugin init() functions before they
/// configure GPIOs or install ISRs. Returns true if the pin is safe to
/// attach on the active board profile (g_boardProfile). On false, logs
/// a Serial line like:
///
///   [rain.pin] init refused: pin not assigned
///   [wind.pin] init refused: GPIO9 = bootstrap pin (boot mode risk)
///
/// `sensorId` and `fieldName` show up verbatim in the log; pass short
/// identifiers (sensor type name + field name). Accepts `int` so plugin
/// code that loaded the value via `cfg["pin"] | -1` can pass directly
/// without an extra cast — values < 0 short-circuit to "not assigned".
bool validateAttachPin(int pin, const char* sensorId, const char* fieldName);

/// Per-sensor "allow restricted pins" override. SensorManager sets this from
/// the sensor's `allow_unsafe_pins` config flag right before calling init(),
/// and clears it afterwards. While set, validateAttachPin() downgrades a
/// strapping- or reserved-pin refusal to a warning and allows the pin (those
/// are usable with correct wiring, e.g. I2C pull-ups). Flash-bus and
/// out-of-range pins are NEVER overridable. Default false.
extern bool g_pinAllowUnsafe;

// --- Persistence -------------------------------------------------------------
//
// The active profile is stored in /board_profile.txt — a tiny key=value
// text file (not config.bin) so a factory reset of user config preserves
// the hardware identity. Format:
//
//   profile=xiao_c3
//   version=1
//
// `version` is the file-schema version (not the profile id); future
// fields can be appended without breaking older firmware.

namespace BoardProfiles {

    /// Reads /board_profile.txt and returns the matching profile pointer,
    /// or nullptr if the file is missing, malformed, or names an unknown
    /// profile. Call once at boot, after initStorage. Idempotent.
    const BoardProfile* load();

    /// Writes the profile shortId to /board_profile.txt atomically. Returns
    /// true on success. Used by the first-run wizard's POST /api/firstrun
    /// handler.
    bool save(const BoardProfile* profile);

}  // namespace BoardProfiles
