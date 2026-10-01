#include "StorageManager.h"
#include "../core/Globals.h"
#include "../utils/Utils.h"
#include <LittleFS.h>
#include "../core/SdCompat.h"   // sdFs() — SD.h only when FEATURE_SD_STORAGE
#include <SPI.h>

// Settings that firmware before configFs() kept on the card when the storage
// was SD, moved to LittleFS once.
//
// On the first boot of this firmware with SD selected and the card mounted,
// the card's copies win over any LittleFS copy: they are the ones the old
// firmware read and wrote, a LittleFS copy is left from before the card.
// If that first boot cannot finish the move (card not mounted, a copy
// failing), it leaves a "pending" marker that records a checksum of each
// LittleFS copy as it was then. From then on settings are read and written on
// LittleFS, so a later attempt overwrites a LittleFS file only if it is still
// that same leftover (or absent), never one saved in the meantime. A "done"
// marker ends it. A device on internal storage is done at once: its LittleFS copies
// are the current ones, and a card selected later must not bring back what
// an older firmware once left on it.
//
// Each file is copied to a temp name, checked against the source size, and
// renamed, so a power cut or a read error mid-copy leaves the previous
// LittleFS file (or none). The card's copies are left in place: nothing
// reads them any more, and deleting a user's file is not this code's call.
static const char SETTINGS_DONE[]    = "/config/.settings_from_sd";
static const char SETTINGS_PENDING[] = "/config/.settings_from_sd.pending";

static bool copySdToLittleFs(fs::FS& sd, const char* path) {
    File src = sd.open(path, FILE_READ);
    if (!src) return false;
    const size_t want = src.size();
    String tmp = String(path) + ".mig";
    File dst = LittleFS.open(tmp, FILE_WRITE);
    bool ok = (bool)dst;
    size_t got = 0;
    uint8_t buf[256];
    while (ok && got < want) {
        const int n = src.read(buf, sizeof(buf));
        if (n <= 0) break;
        ok = dst.write(buf, n) == (size_t)n;
        got += (size_t)n;
    }
    src.close();
    if (dst) dst.close();
    ok = ok && got == want;            // a short read is a failed copy
    if (ok) {
        LittleFS.remove(path);
        ok = LittleFS.rename(tmp, path);
    }
    if (!ok) LittleFS.remove(tmp);
    return ok;
}

static void touch(const char* path) {
    File m = LittleFS.open(path, FILE_WRITE);
    if (m) m.close();
}

// FNV-1a over a LittleFS file, 0 when it does not exist.
static uint32_t lfsChecksum(const char* path) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t h = 2166136261u;
    uint8_t buf[256];
    int n;
    while ((n = f.read(buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
    f.close();
    return h ? h : 1;
}

static void migrateSettingsFromSd() {
    static const char* const FILES[] = {
        "/platform_config.json",
        "/alerts.json",
        "/board_profile.txt",
        "/config/kindle_slots.json",
        "/error_log.txt",
    };
    if (LittleFS.exists(SETTINGS_DONE)) return;
    LittleFS.mkdir("/config");
    if (config.hardware.storageType != STORAGE_SD_CARD) {
        touch(SETTINGS_DONE);
        LittleFS.remove(SETTINGS_PENDING);
        return;
    }
    constexpr size_t N = sizeof(FILES) / sizeof(FILES[0]);
    // The pending marker holds one checksum per FILES entry: the LittleFS
    // copy as it was when the move was first put off (0 = absent).
    uint32_t then[N] = {};
    bool pending = false;
    if (File m = LittleFS.open(SETTINGS_PENDING, FILE_READ)) {
        pending = m.read((uint8_t*)then, sizeof(then)) == sizeof(then);
        m.close();
    }
    auto writePending = [&]() {
        if (pending) return;
        for (size_t i = 0; i < N; i++) then[i] = lfsChecksum(FILES[i]);
        File m = LittleFS.open(SETTINGS_PENDING, FILE_WRITE);
        if (m) { m.write((const uint8_t*)then, sizeof(then)); m.close(); }
    };
    fs::FS* sd = sdFs();
    if (!sd || !sdAvailable) {          // card missing: try again next boot
        writePending();
        return;
    }
    bool all = true;
    for (size_t i = 0; i < N; i++) {
        const char* path = FILES[i];
        if (!sd->exists(path)) continue;
        // Saved on LittleFS since the move was put off: keep it.
        if (pending && lfsChecksum(path) != then[i]) continue;
        if (copySdToLittleFs(*sd, path)) {
            Serial.printf("[storage] %s moved from SD to LittleFS\n", path);
        } else {
            Serial.printf("[storage] could not copy %s to LittleFS\n", path);
            all = false;
        }
    }
    if (all) {
        touch(SETTINGS_DONE);
        LittleFS.remove(SETTINGS_PENDING);
    } else {
        writePending();
    }
}

bool initStorage() {
    DBGLN("Init LittleFS...");
    // R12 / AUDIT 1.7: formatOnFail=FALSE. A transient mount failure used to
    // silently reformat the partition (deleting user config, board profile,
    // and platform_config.json). Now we leave the FS untouched and let the
    // device fall through to safe mode where the user can decide whether to
    // wipe via the failsafe UI (/api/format_filesystem button).
    // Explicitly using "spiffs" label for maximum compatibility on ESP32.
    if (!littleFsAvailable && LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
        littleFsAvailable = true;
    }
    if (littleFsAvailable) {
        DBGLN("LittleFS OK");
    } else {
        DBGLN("LittleFS FAILED! Check partition scheme.");
        littleFsAvailable = false;
    }

    if (config.hardware.storageType == STORAGE_SD_CARD) {
#ifdef FEATURE_SD_STORAGE
        DBGLN("Init SD Card...");
        SPI.begin(config.hardware.pinSdSCK,  config.hardware.pinSdMISO,
                  config.hardware.pinSdMOSI, config.hardware.pinSdCS);
        // Three file slots, not the library's five. The C3 core builds FatFs
        // with 4 KB sectors, and every slot is a FIL with its own 4 KB cache
        // allocated at mount whether a file is open or not: five slots cost
        // ~25 KB of heap, three ~17 KB. The usual load is one data_log append
        // plus perhaps one web download. The data_log trim (Datalog.cpp
        // trim(), once per tenth of maxEntries) holds two, so it fails, and
        // that cycle's rows are not written, only if two other SD files are
        // open at that moment. An open past the limit fails like a missing
        // file. On a C3 the 8 KB is worth that: heap exhaustion here has meant
        // failed TLS and PANIC resets.
        if (SD.begin(config.hardware.pinSdCS, SPI, 4000000, "/sd", 3)) {
            DBGF("SD OK - %llu MB\n", SD.cardSize() / (1024 * 1024));
            sdAvailable = true;
        } else {
            DBGLN("SD FAILED!");
            sdAvailable = false;
        }
#else
        // The config asks for a card but this firmware has no SD support.
        // Say so once and fall through to LittleFS below, rather than
        // leaving the user to wonder why storage silently went internal.
        DBGLN("SD requested, but this build has FEATURE_SD_STORAGE off "
              "- using LittleFS. See src/setup.h.");
        sdAvailable = false;
#endif
    }

    if (littleFsAvailable) migrateSettingsFromSd();

    if (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) {
        activeFS = sdFs();
        fsAvailable = true;
        currentStorageView = "sdcard";
    } else if (littleFsAvailable) {
        activeFS = &LittleFS;
        fsAvailable = true;
        currentStorageView = "internal";
    } else {
        activeFS = nullptr;
        fsAvailable = false;
        Serial.println("ERR: No storage available!");
        return false;
    }
    return true;
}

fs::FS* configFs() {
    if (littleFsAvailable) return &LittleFS;
    return activeFS;
}

fs::FS* getCurrentViewFS() {
    if (currentStorageView == "sdcard" && sdAvailable) return sdFs();
    if (littleFsAvailable) return &LittleFS;
    return nullptr;
}

String getActiveDatalogFile() {
    if (strlen(config.datalog.currentFile) > 0)
        return String(config.datalog.currentFile);
    String folder = String(config.datalog.folder);
    if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
    if (folder.length() > 0 && !folder.endsWith("/"))   folder += "/";
    return folder + String(config.datalog.prefix) + "_datalog.txt";
}

void getStorageInfo(uint64_t& used, uint64_t& total, int& percent,
                    const String& storageType) {
    used = 0; total = 0; percent = 0;
    String sType = storageType;
    if (sType.isEmpty())
        sType = (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable)
                ? "sdcard" : "internal";

    if (sType == "sdcard" && sdAvailable) {
#ifdef FEATURE_SD_STORAGE
        used  = SD.usedBytes();
        total = SD.cardSize();
#endif
    } else if (sType == "internal" && littleFsAvailable) {
        used  = LittleFS.usedBytes();
        total = LittleFS.totalBytes();
    }
    if (total > 0) percent = (used * 100ULL) / total;
}
