#include "StorageManager.h"
#include "../core/Globals.h"
#include "../utils/Utils.h"
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <functional>

bool initStorage() {
    DBGLN("Init LittleFS...");
    if (LittleFS.begin(true)) {
        DBGLN("LittleFS OK");
        littleFsAvailable = true;
    } else {
        DBGLN("LittleFS FAILED!");
        littleFsAvailable = false;
    }

    if (config.hardware.storageType == STORAGE_SD_CARD) {
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
    }

    if (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) {
        activeFS = &SD;
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
    if (currentStorageView == "sdcard" && sdAvailable) return &SD;
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
        used  = SD.usedBytes();
        total = SD.cardSize();
    } else if (sType == "internal" && littleFsAvailable) {
        used  = LittleFS.usedBytes();
        total = LittleFS.totalBytes();
    }
    if (total > 0) percent = (used * 100ULL) / total;
}

String getStorageBarColor(int percent) {
    if (percent >= 90) return config.theme.storageBar90Color;
    if (percent >= 70) return config.theme.storageBar70Color;
    return config.theme.storageBarColor;
}

String generateDatalogFileOptions() {
    if (!fsAvailable || !activeFS) return "<option>No storage</option>";
    String html = "";
    String currentFile = getActiveDatalogFile();

    std::function<void(const String&)> scanDir = [&](const String& path) {
        File dir = activeFS->open(path);
        if (!dir || !dir.isDirectory()) return;
        while (File entry = dir.openNextFile()) {
            String name     = String(entry.name());
            String fullPath = path == "/" ? "/" + name : path + "/" + name;
            if (entry.isDirectory()) {
                scanDir(fullPath);
            } else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) {
                String sel = (fullPath == currentFile) ? "selected" : "";
                html += "<option value='" + fullPath + "' " + sel + ">" + fullPath + "</option>";
            }
            entry.close();
        }
        dir.close();
    };
    scanDir("/");
    return html.length() > 0 ? html : "<option value='/datalog.txt'>datalog.txt</option>";
}

int countDatalogFiles() {
    if (!fsAvailable || !activeFS) return 0;
    int count = 0;
    std::function<void(const String&)> scanDir = [&](const String& path) {
        File dir = activeFS->open(path);
        if (!dir || !dir.isDirectory()) return;
        while (File entry = dir.openNextFile()) {
            String name     = String(entry.name());
            String fullPath = path == "/" ? "/" + name : path + "/" + name;
            if (entry.isDirectory()) scanDir(fullPath);
            else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) count++;
            entry.close();
        }
        dir.close();
    };
    scanDir("/");
    return count;
}
