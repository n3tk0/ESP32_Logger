#include "TrendStore.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <LittleFS.h>

#include "TrendRing.h"

// Two names, because a save that is interrupted must not be able to destroy
// the last good one. The bytes go to the temporary file, that file is closed,
// and only then does it take the real name — the same shape ConfigManager
// uses for the config it cannot afford to lose either.
//
// The CRC inside the snapshot is not made redundant by this. Rename is atomic
// as far as the directory is concerned; it says nothing about whether the
// pages underneath were fully programmed before the supply collapsed.
static const char* TREND_FILE = "/trend.bin";
static const char* TREND_TMP  = "/trend.tmp";

// A failed write buys a minute of quiet rather than retrying from every pass
// of loop(). A full or broken filesystem does not heal inside a millisecond,
// and the log it would produce buries whatever else is wrong. Copied in shape
// from EspNowIngest's saveNodes(), for the same reason and with the same
// signed-difference comparison so it survives the millis() wrap.
static uint32_t s_retryAtMs = 0;

bool trendStoreSave() {
    if (s_retryAtMs != 0 && (int32_t)(s_retryAtMs - millis()) > 0) return false;

    const size_t need = TrendRing::snapshotBytes();

    // On the stack: under 2 KB on a task that has thousands, and a heap
    // allocation on the path that runs when flash is already unhappy is one
    // more thing that can fail while trying to recover.
    uint8_t buf[TrendRing::SNAP_MAX_BYTES];
    if (need > sizeof(buf)) return false;

    if (trendRing.snapshot(buf, sizeof(buf)) != need) return false;

    File f = LittleFS.open(TREND_TMP, "w");
    if (!f) {
        Serial.println("[trend] cannot open the snapshot for writing "
                       "— retrying in 60 s");
        s_retryAtMs = millis() + 60000u;
        if (s_retryAtMs == 0) s_retryAtMs = 1;      // 0 means "no backoff"
        return false;
    }
    const size_t wrote = f.write(buf, need);
    f.close();

    if (wrote != need) {
        Serial.printf("[trend] wrote %u of %u bytes — discarding\n",
                      (unsigned)wrote, (unsigned)need);
        LittleFS.remove(TREND_TMP);
        s_retryAtMs = millis() + 60000u;
        if (s_retryAtMs == 0) s_retryAtMs = 1;
        return false;
    }

    LittleFS.remove(TREND_FILE);
    if (!LittleFS.rename(TREND_TMP, TREND_FILE)) {
        Serial.println("[trend] could not rename the snapshot into place");
        LittleFS.remove(TREND_TMP);
        s_retryAtMs = millis() + 60000u;
        if (s_retryAtMs == 0) s_retryAtMs = 1;
        return false;
    }

    trendRing.clearDirty();
    s_retryAtMs = 0;
    return true;
}

void trendStoreTick() {
    if (!trendRing.dirty()) return;
    trendStoreSave();
}

void trendStoreLoad() {
    if (!LittleFS.exists(TREND_FILE)) return;   // first boot; nothing to say

    File f = LittleFS.open(TREND_FILE, "r");
    if (!f) {
        Serial.println("[trend] the snapshot exists but will not open");
        return;
    }

    const size_t need = TrendRing::snapshotBytes();
    if (f.size() != need) {
        // A layout change between builds, not corruption. Discard rather than
        // reinterpret: reading one struct as another produces series with
        // plausible names and nonsense temperatures.
        Serial.printf("[trend] snapshot is %u bytes, this build wants %u "
                      "— discarding\n", (unsigned)f.size(), (unsigned)need);
        f.close();
        LittleFS.remove(TREND_FILE);
        return;
    }

    uint8_t buf[TrendRing::SNAP_MAX_BYTES];
    const size_t got = f.read(buf, need);
    f.close();

    if (got != need) {
        Serial.println("[trend] snapshot read short — discarding");
        LittleFS.remove(TREND_FILE);
        return;
    }

    if (!trendRing.restore(buf, need)) {
        // The CRC is the interesting case: the file was the right length and
        // still did not survive the last power cut. Saying so is the only way
        // anyone learns that this happened rather than that the chart is new.
        Serial.println("[trend] snapshot failed its own checks — discarding");
        LittleFS.remove(TREND_FILE);
        return;
    }

    Serial.println("[trend] 24-hour chart restored from flash");
}

#endif  // FEATURE_KINDLE_DASHBOARD
