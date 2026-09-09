#include "TrendStore.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <LittleFS.h>

#include "DataPipeline.h"          // fsMutex
#include "TrendRing.h"
#include "../utils/MutexGuard.h"

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

/// Every failure path does the same three things: say what happened, put the
/// dirty flag back so the next tick tries again, and buy the minute. Written
/// once because there are five of them and a path that forgot the markDirty()
/// would leave the hour on the floor without anything looking wrong.
static bool trendSaveFailed(const char* why) {
    Serial.printf("[trend] %s — retrying in 60 s\n", why);
    trendRing.markDirty();
    s_retryAtMs = millis() + 60000u;
    if (s_retryAtMs == 0) s_retryAtMs = 1;      // 0 means "no backoff"
    return false;
}

bool trendStoreSave() {
    const size_t need = TrendRing::snapshotBytes();

    // On the stack: under 2 KB on a task that has thousands, and a heap
    // allocation on the path that runs when flash is already unhappy is one
    // more thing that can fail while trying to recover.
    uint8_t buf[TrendRing::SNAP_MAX_BYTES];
    if (need > sizeof(buf)) return false;      // before the flag is touched

    // CLEARED HERE, WITH THE COPY TAKEN, not after the file is closed.
    //
    // The write below spends tens to hundreds of milliseconds in LittleFS, and
    // ProcessingTask keeps folding readings in throughout. An hour that rolls
    // during that window sets the flag for a roll this snapshot does not
    // contain — and clearing it afterwards discarded that, so nothing was
    // written until the NEXT hour rolled and a power cut in between lost the
    // bucket this whole arrangement exists to keep. Cleared with the copy, the
    // late roll survives as a flag that is still set.
    //
    // Every failure path below puts it back, so a write that does not land
    // still leaves something for the next tick to do. The cost of being wrong
    // the other way is one redundant write.
    trendRing.clearDirty();
    if (trendRing.snapshot(buf, sizeof(buf)) != need) {
        return trendSaveFailed("the snapshot did not come out the expected size");
    }

    // ── fsMutex, and only now ────────────────────────────────────────────────
    //
    // Pillar 1.3: every LittleFS write call site acquires it. This one is
    // called from loop() — trendStoreTick(), and the forced saves on the
    // restart and deep-sleep paths — while StorageTask is appending rows to
    // the day's CSV and the async web server is serving files off the same
    // filesystem. LittleFS is not re-entrant across tasks: two writers in its
    // metadata at once corrupts the directory or panics the core, and neither
    // failure names the code that caused it.
    //
    // TAKEN AFTER THE COPY, not around it. snapshot() takes the ring's own
    // spinlock; nothing can block inside a critical section, so the two could
    // not deadlock in either order — but held around the copy, fsMutex would
    // be held for work that is not filesystem work, and fsMutex is the lock
    // every writer on the device queues behind. The copy is in hand by the
    // time the file is opened, so the lock covers the filesystem and nothing
    // else.
    //
    // A failed take is fatal to this attempt, never a reason to write anyway
    // (Pillar 1.2). The one case that cannot be recovered is a force-deleted
    // task that died holding it — see TaskManager::shutdown() — and writing
    // into a filesystem whose last writer was killed mid-operation is the one
    // thing worse than losing the hour.
    MutexGuard guard(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !guard.isLocked()) {
        return trendSaveFailed("fsMutex timeout");
    }

    File f = LittleFS.open(TREND_TMP, "w");
    if (!f) {
        return trendSaveFailed("cannot open the snapshot for writing");
    }
    const size_t wrote = f.write(buf, need);
    f.close();

    if (wrote != need) {
        LittleFS.remove(TREND_TMP);
        char why[64];
        snprintf(why, sizeof(why), "wrote %u of %u bytes",
                 (unsigned)wrote, (unsigned)need);
        return trendSaveFailed(why);
    }

    // RENAME FIRST, and only unlink if it will not go over the top.
    //
    // This used to remove the good file and then rename onto the name it had
    // just freed — which opens a window in which NEITHER name holds a
    // snapshot. A power cut in that window is the exact event the two-file
    // dance exists to survive, and it left the collector waking with a blank
    // chart: the failure the whole feature was written to remove, caused by
    // the code written to prevent it.
    //
    // littlefs's rename replaces an existing target atomically, so the unlink
    // bought nothing. It is kept as a fallback because the filesystem wrapper
    // is not the same on every core, and a rename that refuses an existing
    // name would otherwise mean the snapshot could never be updated at all.
    if (!LittleFS.rename(TREND_TMP, TREND_FILE)) {
        LittleFS.remove(TREND_FILE);
        if (!LittleFS.rename(TREND_TMP, TREND_FILE)) {
            LittleFS.remove(TREND_TMP);
            return trendSaveFailed("could not rename the snapshot into place");
        }
    }

    s_retryAtMs = 0;
    return true;
}

void trendStoreTick() {
    if (!trendRing.dirty()) return;
    // THE BACKOFF LIVES HERE, not in trendStoreSave(). A failed write buys a
    // minute of quiet rather than retrying from every pass of loop(): a full
    // or broken filesystem does not heal inside a millisecond, and the log it
    // would produce buries whatever else is wrong. The forced save is the
    // opposite case — it is called when there is no next attempt — so it does
    // not consult this.
    if (s_retryAtMs != 0 && (int32_t)(s_retryAtMs - millis()) > 0) return;
    trendStoreSave();
}

void trendStoreLoad() {
    // fsMutex IS NULL HERE, AND THAT IS CORRECT. This runs from setup(),
    // before _initPlatform() calls TaskManager::init() — the call that creates
    // the mutex and starts the tasks it guards against. A null handle means
    // "there is no concurrency to guard against yet, proceed", the same
    // documented Pillar 1.3 exemption ConfigManager::saveConfig() takes on the
    // migration path. Only a FAILED take is fatal, and one can only happen if
    // this is ever called later, from somewhere that does have company.
    //
    // Held across restore() as well as the reads: restore() takes the ring's
    // spinlock, which cannot block, and splitting the function to release the
    // mutex in between would leave the removes below outside it.
    MutexGuard guard(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !guard.isLocked()) {
        Serial.println("[trend] fsMutex timeout — the chart starts empty");
        return;
    }

    // A save that was cut short leaves the temporary file behind, and nothing
    // else ever looks at that name again — so without this it sits on the
    // filesystem for the life of the device, one snapshot's worth of a part
    // that has a few hundred kilobytes in total. Removed here rather than in
    // save(), because this is the one place that runs after a crash.
    if (LittleFS.exists(TREND_TMP)) LittleFS.remove(TREND_TMP);

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

    // Under 2 KB on the stack, and it is the only large frame on this path:
    // restore() reads the slots one at a time out of this buffer rather than
    // staging a second copy of them beside it.
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
