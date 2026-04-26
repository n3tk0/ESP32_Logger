#include "OtaManager.h"
#include "../core/Globals.h"
#include "../setup.h"               // OTA_CONFIRM_TIMEOUT_MS
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <LittleFS.h>

// OTA_CONFIRM_TIMEOUT_MS is configured in setup.h (default 90 s).
// It controls how long the new firmware must run without crashing before
// the watchdog flips the OTA app slot to "valid".  Until then a panic or
// hardware-watchdog reset triggers a bootloader rollback to the previous
// slot on next boot.

static bool s_pending       = false;
static bool s_confirmed     = false;
static bool s_rollbackCapable = false;   // true only if bootloader supports rollback
static uint32_t s_pendingDeadline = 0;   // millis() when watchdog auto-confirms

// Cache partition labels so they survive the lifetime of the program
static char s_runningLabel[8]  = "";
static char s_previousLabel[8] = "";

// ---------------------------------------------------------------------------
static void _logOtaEvent(const char* event) {
    if (!littleFsAvailable) return;
    File f = LittleFS.open("/reset_log.txt", FILE_APPEND);
    if (!f) return;
    char line[80];
    snprintf(line, sizeof(line), "boot#%u  OTA_%s  running=%s\n",
             (unsigned)bootCount, event, s_runningLabel);
    f.print(line);
    f.close();
}

// ---------------------------------------------------------------------------
void OtaManager::boot() {
    // Cache running partition label
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->label) {
        strncpy(s_runningLabel, running->label, sizeof(s_runningLabel) - 1);
    }

    // Cache previous (rollback target) partition label
    const esp_partition_t* prev = esp_ota_get_last_invalid_partition();
    if (!prev) {
        // No invalid partition — try next OTA slot as previous
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        if (next && next->label) {
            strncpy(s_previousLabel, next->label, sizeof(s_previousLabel) - 1);
        }
    } else if (prev->label) {
        strncpy(s_previousLabel, prev->label, sizeof(s_previousLabel) - 1);
    }

    // Check if current firmware needs verification.
    // If the bootloader was NOT built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    // (default for Arduino IDE pre-built bootloaders), the state will always be
    // ESP_OTA_IMG_VALID or ESP_OTA_IMG_UNDEFINED — rollback-on-crash is inactive
    // but manual rollback via rollback() still works.
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_pending          = true;
            s_rollbackCapable  = true;
            s_pendingDeadline  = millis() + (uint32_t)OTA_CONFIRM_TIMEOUT_MS;
            Serial.printf("[OTA] Firmware pending verify on %s — confirming in %us\n",
                          s_runningLabel,
                          (unsigned)(OTA_CONFIRM_TIMEOUT_MS / 1000));
            _logOtaEvent("PENDING_VERIFY");
        } else if (state == ESP_OTA_IMG_VALID) {
            s_confirmed = true;
            DBGF("[OTA] Firmware on %s already confirmed\n", s_runningLabel);
        } else {
            // ESP_OTA_IMG_UNDEFINED / ESP_OTA_IMG_NEW etc.
            s_confirmed = true;  // no rollback-capable bootloader, treat as valid
            DBGF("[OTA] Partition state %d — rollback-on-crash unavailable\n", state);
        }
    } else {
        // Single app partition or esp_ota API unavailable
        s_confirmed = true;
        DBGF("[OTA] Partition state query failed — rollback disabled\n");
    }

    // Note: we deliberately do NOT auto-confirm here.  Confirming inside
    // setup() means the slot is marked valid before the device has run
    // long enough to demonstrate stability — a firmware that crashes 30 s
    // after boot would never get rolled back.  tick() handles confirmation
    // once OTA_CONFIRM_TIMEOUT_MS has actually elapsed.
}

// ---------------------------------------------------------------------------
void OtaManager::tick(uint32_t nowMs) {
    if (s_confirmed || !s_pending) return;
    // millis() wraps at ~49.7 days; using signed difference makes the
    // comparison wrap-safe (positive ⇒ deadline still in the future,
    // ≤ 0 ⇒ deadline reached/passed).
    int32_t remaining = (int32_t)(s_pendingDeadline - nowMs);
    if (remaining <= 0) {
        Serial.printf("[OTA] Stability window elapsed (%us) — confirming\n",
                      (unsigned)(OTA_CONFIRM_TIMEOUT_MS / 1000));
        confirm();
    }
}

// ---------------------------------------------------------------------------
uint32_t OtaManager::millisUntilConfirm() {
    if (s_confirmed || !s_pending) return 0;
    int32_t remaining = (int32_t)(s_pendingDeadline - millis());
    return (remaining > 0) ? (uint32_t)remaining : 0;
}

// ---------------------------------------------------------------------------
bool OtaManager::confirm() {
    if (s_confirmed) return true;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_confirmed       = true;
        s_pending         = false;
        s_pendingDeadline = 0;
        Serial.printf("[OTA] Firmware confirmed on %s\n", s_runningLabel);
        _logOtaEvent("CONFIRMED");
        return true;
    }

    Serial.printf("[OTA] Confirm failed: %s\n", esp_err_to_name(err));
    return false;
}

// ---------------------------------------------------------------------------
bool OtaManager::rollback() {
    // Verify a previous partition exists before attempting rollback
    const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
    if (!prev) {
        Serial.println("[OTA] Rollback impossible — no alternate partition");
        return false;
    }

    Serial.printf("[OTA] Rolling back from %s to %s\n",
                  s_runningLabel, s_previousLabel);
    _logOtaEvent("ROLLBACK");

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // If we get here, rollback failed (reboot didn't happen)
    Serial.printf("[OTA] Rollback failed: %s\n", esp_err_to_name(err));
    return false;
}

// ---------------------------------------------------------------------------
bool OtaManager::isPendingVerify() {
    return s_pending && !s_confirmed;
}

// ---------------------------------------------------------------------------
const char* OtaManager::runningPartitionLabel() {
    return s_runningLabel;
}

// ---------------------------------------------------------------------------
const char* OtaManager::previousPartitionLabel() {
    return s_previousLabel;
}

bool OtaManager::isRollbackCapable() {
    return s_rollbackCapable;
}
