// ============================================================================
// src/modules/UsbCdcModule.cpp — USB CDC configuration management
// ============================================================================

#include "UsbCdcModule.h"
#include <Preferences.h>
#include <esp_chip_info.h>

// Global instance
UsbCdcModule usbCdc;

// ── Constants ──────────────────────────────────────────────────────────────
static const char* NVS_NAMESPACE = "usb_cdc";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_FIRST_RUN = "first_run";

// ── Implementation ─────────────────────────────────────────────────────────

void UsbCdcModule::begin() {
    if (!isUsbCdcSupported()) {
        Serial.println("[UsbCdc] This board does not support USB CDC configuration.");
        return;
    }

    loadFromNvs();

    if (first_run_) {
        Serial.println();
        Serial.println("═══════════════════════════════════════════════════════");
        Serial.println("                   FIRST RUN SETUP");
        Serial.println("═══════════════════════════════════════════════════════");
        firstRunSetup();
    }

    printStatus();
}

void UsbCdcModule::printStatus() const {
    if (!isUsbCdcSupported()) {
        return;
    }

    // Show actual hardware state from build-time flag, not NVS preference
    bool hwEnabled = false;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT == 1
    hwEnabled = true;
#endif

    String board = getBoardName();
    String status = hwEnabled ? "ON (locked for serial)" : "OFF (GPIO available)";
    if (hwEnabled != enabled_) {
        status += " [Recompile required to apply change]";
    }
    String pins = getAffectedPins();

    Serial.println();
    Serial.println("┌─ USB CDC Configuration ─────────────────────────────────");
    Serial.printf("│ Board:            %s\n", board.c_str());
    Serial.printf("│ USB CDC on boot:  %s\n", status.c_str());
    if (!pins.isEmpty()) {
        Serial.printf("│ Affected pins:    %s\n", pins.c_str());
    }
    Serial.println("│");
    if (hwEnabled) {
        Serial.println("│ ✓ Serial over USB available (easy debugging)");
        Serial.println("│ ✗ GPIO pins locked for USB communication");
    } else {
        Serial.println("│ ✓ GPIO pins available for sensors/IO");
        Serial.println("│ ✗ No USB serial (use HTTP or UART for logs)");
    }
    Serial.println("└─────────────────────────────────────────────────────────");
    Serial.println();
}

void UsbCdcModule::setUsbCdcEnabled(bool enabled) {
    if (!isUsbCdcSupported()) return;
    if (enabled == enabled_) return;   // unchanged → skip the NVS write
    saveToNvs(enabled);                // persists to NVS + updates enabled_
}

void UsbCdcModule::syncFromNvs() {
    if (!isUsbCdcSupported()) return;
    loadFromNvs();
}

bool UsbCdcModule::isUsbCdcActiveAtBoot() const {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT == 1
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// The USB pins are a property of the SILICON, not of the carrier board.
//
// This used to test board macros — ARDUINO_SEEED_XIAO_ESP32C3,
// ARDUINO_ESP32C3_DEV, ARDUINO_ESP32S3_DEV — and answered "no USB pins, none
// locked" for every board outside that list. Both XIAO targets fell outside
// it, verified with a #pragma message probe on each env rather than by
// reading board JSON:
//
//   xiao_esp32c3       ARDUINO_XIAO_ESP32C3          no match  -> INERT
//   xiao_esp32s3       ARDUINO_XIAO_ESP32S3          no match  -> INERT
//   lolin_c3_pico      (its own)                     no match  -> would be
//   esp32c3_supermini  ARDUINO_ESP32C3_DEV           matched   -> worked
//   esp32s3(_n16r8)    ARDUINO_ESP32S3_DEV           matched   -> worked
//
// Seeed's macro is ARDUINO_XIAO_ESP32C3, with no SEEED_ — so the very board
// the first branch was written for never matched it. On those the UI reported
// the USB D-/D+ pair as free while USB CDC was compiled in and holding it,
// which is precisely the "sensor never answers and never logs why" failure
// the board profiles exist to prevent, and every new board would have
// re-introduced it.
//
// USB Serial/JTAG is fixed in the pad ring of each part:
//   ESP32-C3:  GPIO18 = D-, GPIO19 = D+
//   ESP32-S3:  GPIO19 = D-, GPIO20 = D+
// so the chip-family macro is both the correct question and one no new board
// can get wrong.
//
// The paired string forms are literals rather than runtime concatenations:
// String() + int pulls in the conversion machinery at every call site, and on
// the 4 MB C3 the all-features image has about ten kilobytes of headroom.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#  define LOGGER_USB_DM       18
#  define LOGGER_USB_DP       19
#  define LOGGER_USB_PINS_CSV "18,19"
#  define LOGGER_USB_PINS_TXT "GPIO 18, 19 (USB D-/D+)"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#  define LOGGER_USB_DM       19
#  define LOGGER_USB_DP       20
#  define LOGGER_USB_PINS_CSV "19,20"
#  define LOGGER_USB_PINS_TXT "GPIO 19, 20 (USB D-/D+)"
#endif

String UsbCdcModule::getBoardName() const {
    // ARDUINO_BOARD is set by every board definition in the core, so this
    // names the actual board rather than falling back to "Unknown ESP32" for
    // anything not on a hand-maintained list.
#if defined(ARDUINO_BOARD)
    return ARDUINO_BOARD;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    return "ESP32-C3";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return "ESP32-S3";
#else
    return "Unknown ESP32";
#endif
}

String UsbCdcModule::getAffectedPins() const {
#if defined(LOGGER_USB_DM)
    return LOGGER_USB_PINS_TXT;
#else
    return "";
#endif
}

bool UsbCdcModule::isUsbPinLocked(int pin) const {
    // Actual pin lock status is determined by the build-time flag ARDUINO_USB_CDC_ON_BOOT
    // NVS preference (enabled_) is for future recompiles, not current hardware state
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT != 1)
    return false;  // USB CDC disabled at build time, no pins locked
#elif defined(LOGGER_USB_DM)
    return (pin == LOGGER_USB_DM || pin == LOGGER_USB_DP);
#else
    return false;
#endif
}

String UsbCdcModule::getUsbPins() const {
#if defined(LOGGER_USB_DM)
    return LOGGER_USB_PINS_CSV;
#else
    return "";
#endif
}

bool UsbCdcModule::isUsbCdcSupported() const {
#if defined(LOGGER_USB_DM)
    return true;
#else
    return false;
#endif
}

String UsbCdcModule::detectBoard() const {
    return getBoardName();
}

void UsbCdcModule::loadFromNvs() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // true = read-only

    // Load enabled flag (default: true for USB CDC on boot)
    enabled_ = prefs.getBool(NVS_KEY_ENABLED, true);

    // Load first-run flag (default: true for new devices)
    first_run_ = prefs.getBool(NVS_KEY_FIRST_RUN, true);

    prefs.end();
}

void UsbCdcModule::saveToNvs(bool enabled) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);  // false = read-write

    enabled_ = enabled;
    prefs.putBool(NVS_KEY_ENABLED, enabled);
    prefs.putBool(NVS_KEY_FIRST_RUN, false);  // Mark first run as complete

    prefs.end();
}

void UsbCdcModule::firstRunSetup() {
    String board = getBoardName();
    String pins = getAffectedPins();

    Serial.println();
    Serial.printf("Board detected: %s\n", board.c_str());
    if (!pins.isEmpty()) {
        Serial.printf("USB pins: %s\n", pins.c_str());
    }
    Serial.println();
    Serial.println("Choose USB CDC configuration:");
    Serial.println("  [1] Enable USB CDC (default)");
    Serial.println("      ✓ Easy serial debugging via USB cable");
    Serial.println("      ✗ USB pins locked for communication");
    Serial.println();
    Serial.println("  [2] Disable USB CDC");
    Serial.println("      ✓ USB pins available for GPIO/sensors");
    Serial.println("      ✗ No USB serial (use HTTP or UART logs)");
    Serial.println();
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    Serial.println("Current setting: USB CDC ON (build time)");
#else
    Serial.println("Current setting: USB CDC OFF (build time)");
#endif
    Serial.println("Enter 1 or 2 (default 1): ");

    // Wait for user input with timeout
    unsigned long start = millis();
    unsigned long timeout = 30000;  // 30 second timeout
    String input = "";

    while (millis() - start < timeout) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                break;
            }
            if (c >= '0' && c <= '9') {
                input += c;
                Serial.print(c);
            }
        }
        delay(10);
    }

    Serial.println();

    bool choice = (input == "2") ? false : true;  // Default to true (USB CDC on)

    if (choice) {
        Serial.println("✓ USB CDC enabled. Pins 18/19 are locked for USB.");
    } else {
        Serial.println("✓ USB CDC disabled. Pins 18/19 are available for GPIO.");
        Serial.println("  NOTE: Next firmware compilation must have USB CDC OFF.");
    }

    saveToNvs(choice);
}
