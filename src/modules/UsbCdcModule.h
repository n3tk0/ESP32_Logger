// ============================================================================
// src/modules/UsbCdcModule.h — USB CDC on-boot configuration management
// ============================================================================
// Manages USB CDC (serial over USB) on-boot configuration for boards that
// support it (ESP32-C3, ESP32-S3).
//
// Features:
//  • First-run detection and interactive setup
//  • Non-volatile storage of user preference
//  • Runtime querying of current configuration
//  • Board-aware (ESP32-C3, ESP32-S3, etc.)
//
// Storage:
//  • Uses ESP32 NVS (Non-Volatile Storage) via Preferences
//  • Persists across firmware updates and reboots
//
// Usage:
//  UsbCdcModule usb;
//  usb.begin();  // Call in setup() after Serial.begin()
//  bool enabled = usb.isUsbCdcEnabled();
//  usb.printStatus();
// ============================================================================

#pragma once

#include <Arduino.h>

class UsbCdcModule {
public:
    UsbCdcModule() : enabled_(false), first_run_(false) {}

    /// Initialize USB CDC configuration. Call in setup() after Serial.begin()
    void begin();

    /// Check if USB CDC on boot is currently enabled
    bool isUsbCdcEnabled() const { return enabled_; }

    /// Check if a specific pin is locked by USB CDC (for validatePin integration)
    /// Returns true if pin is reserved for USB and USB CDC is enabled
    bool isUsbPinLocked(int pin) const;

    /// Get the list of USB-reserved pins for this board (as string for logging)
    String getUsbPins() const;

    /// Print current USB CDC status and pin information to Serial
    void printStatus() const;

    /// Get board name (ESP32-C3 SuperMini, XIAO ESP32-C3, ESP32-S3, etc.)
    String getBoardName() const;

    /// Get affected pins for this board when USB CDC is enabled
    String getAffectedPins() const;

    /// Check if this board supports USB CDC configuration
    bool isUsbCdcSupported() const;

    /// Get first-run status (true if firmware is running for first time on this device)
    bool isFirstRun() const { return first_run_; }

    /// Persist the USB-CDC-on-boot preference to NVS. Takes effect on the next
    /// firmware recompile/flash (the actual mode is a compile-time build flag).
    void setUsbCdcEnabled(bool enabled);

    /// Reload the cached preference from NVS. begin() (which also runs the
    /// interactive first-run setup) is not called in the headless runtime, so
    /// the module manager calls this to read the stored preference on demand.
    void syncFromNvs();

    /// True if USB CDC is actually active on THIS build (compile-time
    /// ARDUINO_USB_CDC_ON_BOOT flag), independent of the NVS preference.
    bool isUsbCdcActiveAtBoot() const;

private:
    bool enabled_;
    bool first_run_;

    /// Detect board type based on build macros and chip ID
    String detectBoard() const;

    /// Load USB CDC setting from NVS (Non-Volatile Storage)
    void loadFromNvs();

    /// Save USB CDC setting to NVS
    void saveToNvs(bool enabled);

    /// Perform first-run interactive setup (ask user about USB CDC preference)
    void firstRunSetup();
};

// Global instance (similar to how Serial is global)
extern UsbCdcModule usbCdc;
