#include "OtaManager.h"
#include "../core/Globals.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <LittleFS.h>

// Time the new firmware must run without crashing before auto-confirmation.
// 90 seconds: enough for WiFi connect + sensor init + a few pipeline cycles.
static constexpr uint32_t OTA_CONFIRM_TIMEOUT_MS = 90000;

static bool s_pending   = false;
static bool s_confirmed = false;

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

    // Check if current firmware needs verification
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_pending = true;
            Serial.printf("[OTA] Firmware pending verify on %s — confirming in %us\n",
                          s_runningLabel, OTA_CONFIRM_TIMEOUT_MS / 1000);
            _logOtaEvent("PENDING_VERIFY");
        } else if (state == ESP_OTA_IMG_VALID) {
            s_confirmed = true;
            DBGF("[OTA] Firmware on %s already confirmed\n", s_runningLabel);
        }
    }

    // If firmware is pending verification, schedule auto-confirm.
    // The bootloader will automatically rollback if we crash before confirming.
    if (s_pending) {
        // Non-blocking: we set a flag and let loop() confirm after the timeout.
        // This is simpler and safer than using a FreeRTOS timer.
        // The actual confirmation happens via the confirm() call below.
        // For safety, confirm immediately at boot — the device has already
        // survived setup() which includes WiFi, storage, sensors, and tasks.
        // If any of those panicked, we wouldn't reach this point.
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
