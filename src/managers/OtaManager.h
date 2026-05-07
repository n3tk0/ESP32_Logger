#pragma once
#include <Arduino.h>

// ============================================================================
// OtaManager — OTA rollback support
//
// Uses ESP-IDF's app partition API to manage firmware updates with automatic
// rollback on crash.  The partition table must have ota_0 + ota_1 + otadata.
//
// Lifecycle:
//   1. OTA update writes to the inactive partition and marks it "pending verify"
//   2. On next boot, otaCheckAndConfirm() runs:
//      a. If pending verify: starts a safety timer. If the device survives
//         OTA_CONFIRM_TIMEOUT_MS without panic/WDT, the firmware is confirmed.
//      b. If the device crashes before confirmation, the bootloader
//         automatically rolls back to the previous partition on next boot.
//   3. Manual rollback via /api/ota/rollback reverts to the previous partition.
//
// ============================================================================
namespace OtaManager {

    // Call once in setup() after basic init.  Captures partition state and
    // arms the rollback watchdog when the running image is in PENDING_VERIFY.
    // Does NOT auto-confirm — that happens later from tick() once the
    // OTA_CONFIRM_TIMEOUT_MS stability window has elapsed.
    void boot();

    // Pump the rollback watchdog from the main loop.  No-op once the image
    // is confirmed.  Confirms automatically when millis() ≥ deadline.
    void tick(uint32_t nowMs);

    // Mark current firmware as confirmed (valid).
    // Called automatically by tick() once the timeout elapses, or manually
    // via the /api/ota/confirm endpoint.
    bool confirm();

    // Milliseconds remaining until the watchdog auto-confirms.  Returns 0
    // when not pending or already confirmed; lets the UI surface a
    // countdown after a fresh OTA install.
    uint32_t millisUntilConfirm();

    // Roll back to the previous OTA partition and restart.
    // Returns false only if rollback is not possible (e.g. single app partition).
    bool rollback();

    // True if current firmware is pending verification (not yet confirmed).
    bool isPendingVerify();

    // Returns the label of the currently running partition (e.g. "app0" or "app1").
    const char* runningPartitionLabel();

    // Returns the label of the partition that would be rolled back to, or "" if N/A.
    const char* previousPartitionLabel();

    // True if the bootloader supports automatic rollback-on-crash.
    // False when using Arduino IDE's default pre-built bootloader.
    // Manual rollback via rollback() still works regardless.
    bool isRollbackCapable();
}
