#include "HybridStorage.h"
#include "../core/Globals.h"   // config.hardware.pinSdCS, fsAvailable, activeFS

HybridStorage::Mode HybridStorage::_mode  = HybridStorage::LITTLEFS;
bool                HybridStorage::_sdOk  = false;
uint8_t             HybridStorage::_sdCS  = 10;

// ---------------------------------------------------------------------------
bool HybridStorage::begin(const char* cfgPath) {
    // Read cloud_only flag from platform_config.json
    bool cloudOnly = false;
    _sdCS = config.hardware.pinSdCS;

    if (fsAvailable && activeFS && activeFS->exists(cfgPath)) {
        File f = activeFS->open(cfgPath, FILE_READ);
        if (f) {
            JsonDocument doc;
            if (deserializeJson(doc, f) == DeserializationError::Ok) {
                cloudOnly = doc["storage"]["cloud_only"] | false;
            }
            f.close();
        }
    }

    if (cloudOnly) {
        _mode = CLOUD;
        DBGLN("[HybridStorage] Cloud-only mode — no local writes");
        return true;
    }

    // Try SD card on configured CS pin
    _sdOk = SD.begin(_sdCS);
    if (_sdOk) {
        _mode = HYBRID;
        DBGF("[HybridStorage] SD ready (CS=%d) — hybrid mode\n", _sdCS);
        // Ensure /logs directory exists on SD
        ensureDir(SD, "/logs");
    } else {
        _mode = LITTLEFS;
        DBGLN("[HybridStorage] No SD — LittleFS-only mode");
        if (fsAvailable && activeFS) {
            ensureDir(*activeFS, "/logs");
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
fs::FS* HybridStorage::primary() {
    if (_mode == CLOUD) return nullptr;
    if (_mode == HYBRID && _sdOk) return &SD;
    return fsAvailable ? activeFS : nullptr;
}

// ---------------------------------------------------------------------------
fs::FS* HybridStorage::secondary() {
    if (_mode != HYBRID) return nullptr;
    return fsAvailable ? activeFS : nullptr; // LittleFS as cache
}

// ---------------------------------------------------------------------------
void HybridStorage::mirrorWrite(const char* path, const uint8_t* data, size_t len) {
    // Write to primary
    fs::FS* pri = primary();
    if (pri) {
        File f = pri->open(path, FILE_APPEND);
        if (f) {
            size_t written = f.write(data, len);
            if (written != len) DBGF("[HybridStorage] WARN: primary wrote %u/%u\n", written, len);
            f.close();
        }
    }
    // Mirror to secondary (cache, best-effort — never blocks on failure)
    fs::FS* sec = secondary();
    if (sec) {
        File f = sec->open(path, FILE_APPEND);
        if (f) {
            size_t written = f.write(data, len);
            if (written != len) DBGF("[HybridStorage] WARN: mirror wrote %u/%u\n", written, len);
            f.close();
        }
    }
}

// ---------------------------------------------------------------------------
void HybridStorage::ensureDir(fs::FS& fs, const char* dir) {
    if (!fs.exists(dir)) {
        fs.mkdir(dir);
    }
}
