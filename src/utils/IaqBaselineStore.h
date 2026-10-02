// ============================================================================
// src/utils/IaqBaselineStore.h
//
// Keeps a GasIaq baseline across restarts in a small LittleFS file, so IAQ
// does not start over from whatever the air was at boot. Used by
// BME688Sensor (a wired sensor) and RemoteNodeSensor (a node's BME688).
//
// The file holds the heater settings next to the baseline: the MOX
// resistance depends on the heater temperature and duration, so a baseline
// taken under other settings is discarded rather than trusted.
//
// Firmware only; the host tests link their own stubs of these functions.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace IaqBaselineStore {

/// "/config/<prefix>_<hash of id>.bin". The id is hashed rather than put in
/// the path because ids are user text.
void path(const char* prefix, const char* id, char* out, size_t len);

/// The stored baseline when the file exists, is intact and was taken under
/// these heater settings; 0 otherwise. `tag` names the caller in the log.
float load(const char* path, int heaterTemp, int heaterDurMs, const char* tag);

/// Write the baseline with these heater settings. Waits at most `waitMs` for
/// fsMutex; false when it is busy or the write fails.
bool save(const char* path, int heaterTemp, int heaterDurMs, float baseline,
          uint32_t waitMs);

/// When to save: at most once per SAVE_EVERY_MS, only once the baseline is
/// worth keeping, and not for a change of under 2 % (flash wear for nothing).
/// A failed save is retried after RETRY_MS instead of waiting out the hour.
struct Saver {
    static constexpr uint32_t SAVE_EVERY_MS = 60UL * 60UL * 1000UL;
    static constexpr uint32_t RETRY_MS      =  5UL * 60UL * 1000UL;

    float    saved  = 0.0f;   // value last written (or restored)
    uint32_t lastMs = 0;      // last attempt

    /// Restart the hour from `now` and forget what was written.
    void reset(uint32_t now, float restored) { saved = restored; lastMs = now; }

    /// True when `baseline` should be written now; marks the attempt.
    bool due(uint32_t now, float baseline) {
        if (now - lastMs < SAVE_EVERY_MS) return false;
        lastMs = now;
        if (baseline < 1.0f) return false;
        if (saved > 0.0f) {
            const float d = baseline > saved ? baseline - saved : saved - baseline;
            if (d < saved * 0.02f) return false;
        }
        return true;
    }

    void done(uint32_t now, float baseline, bool ok) {
        if (ok) saved = baseline;
        else    lastMs = now - SAVE_EVERY_MS + RETRY_MS;
    }
};

} // namespace IaqBaselineStore
