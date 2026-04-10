#include "OtaManager.h"
#include "../core/Globals.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <LittleFS.h>

// Time the new firmware must run without crashing before auto-confirmation.
// 90 seconds: enough for WiFi connect + sensor init + a few pipeline cycles.
static constexpr uint32_t OTA_CONFIRM_TIMEOUT_MS = 90000;

static bool s_pending       = false;
static bool s_confirmed     = false;
static bool s_rollbackCapable = false;   // true only if bootloader supports rollback

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
            s_pending         = true;
            s_rollbackCapable = true;
            Serial.printf("[OTA] Firmware pending verify on %s — auto-confirming\n",
                          s_runningLabel);
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

    // If firmware is pending verification, confirm now — we survived setup()
    // (WiFi, storage, sensors, tasks). A crash before this point triggers
    // automatic bootloader rollback.
    if (s_pending) {
        confirm();
    }
}

// ---------------------------------------------------------------------------
bool OtaManager::confirm() {
    if (s_confirmed) return true;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_confirmed = true;
        s_pending   = false;
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
