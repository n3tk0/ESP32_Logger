#include "StorageManager.h"
#include "../core/Globals.h"
#include "../utils/Utils.h"
#include <LittleFS.h>
#include "../core/SdCompat.h"   // sdFs() — SD.h only when FEATURE_SD_STORAGE
#include <SPI.h>

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
        if (SD.begin(config.hardware.pinSdCS)) {
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
