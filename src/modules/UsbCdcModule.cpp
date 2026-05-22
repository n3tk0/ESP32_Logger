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

    String board = getBoardName();
    String status = enabled_ ? "ON (locked for serial)" : "OFF (GPIO available)";
    String pins = getAffectedPins();

    Serial.println();
    Serial.println("┌─ USB CDC Configuration ─────────────────────────────────");
    Serial.printf("│ Board:            %s\n", board.c_str());
    Serial.printf("│ USB CDC on boot:  %s\n", status.c_str());
    if (!pins.isEmpty()) {
        Serial.printf("│ Affected pins:    %s\n", pins.c_str());
    }
    Serial.println("│");
    if (enabled_) {
        Serial.println("│ ✓ Serial over USB available (easy debugging)");
        Serial.println("│ ✗ GPIO pins locked for USB communication");
    } else {
        Serial.println("│ ✓ GPIO pins available for sensors/IO");
        Serial.println("│ ✗ No USB serial (use HTTP or UART for logs)");
    }
    Serial.println("└─────────────────────────────────────────────────────────");
    Serial.println();
}

String UsbCdcModule::getBoardName() const {
#if defined(ARDUINO_SEEED_XIAO_ESP32C3)
    return "Seeed XIAO ESP32-C3";
#elif defined(ARDUINO_ESP32C3_DEV)
    return "Generic ESP32-C3";
#elif defined(ARDUINO_ESP32S3_DEV)
    return "Generic ESP32-S3";
#else
    return "Unknown ESP32";
#endif
}

String UsbCdcModule::getAffectedPins() const {
#if defined(ARDUINO_SEEED_XIAO_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
    return "GPIO 18, 19 (USB D+/D-)";
#elif defined(ARDUINO_ESP32S3_DEV)
    return "GPIO 19, 20 (USB D+/D-)";
#else
    return "";
#endif
}

bool UsbCdcModule::isUsbPinLocked(int pin) const {
    if (!enabled_) {
        return false;  // USB CDC disabled, no pins locked
    }

#if defined(ARDUINO_SEEED_XIAO_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
    return (pin == 18 || pin == 19);
#elif defined(ARDUINO_ESP32S3_DEV)
    return (pin == 19 || pin == 20);
#else
    return false;
#endif
}

String UsbCdcModule::getUsbPins() const {
#if defined(ARDUINO_SEEED_XIAO_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
    return "18,19";
#elif defined(ARDUINO_ESP32S3_DEV)
    return "19,20";
#else
    return "";
#endif
}

bool UsbCdcModule::isUsbCdcSupported() const {
#if defined(ARDUINO_SEEED_XIAO_ESP32C3) || defined(ARDUINO_ESP32C3_DEV) || defined(ARDUINO_ESP32S3_DEV)
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
    Serial.println("Current setting: USB CDC ON (build time)");
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
