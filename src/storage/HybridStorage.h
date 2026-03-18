#pragma once
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>

// ============================================================================
// HybridStorage — transparent SD + LittleFS routing
//
// Priority:  1. SD card  → primary storage (full logs)
//            2. LittleFS → fallback / cache for fast web reads
//            3. cloud_only mode → skip local writes
//
// After initStorage() (existing), call HybridStorage::begin() to get the
// active filesystem pointer.  All new JsonLogger and TaskManager code uses
// this pointer; existing DataLogger continues to use `activeFS` directly.
//
// Usage:
//   HybridStorage::begin();
//   fs::FS* fs = HybridStorage::primary();
// ============================================================================
class HybridStorage {
public:
    enum Mode : uint8_t {
        HYBRID    = 0,   // SD primary, LittleFS cache
        LITTLEFS  = 1,   // No SD, LittleFS only
        CLOUD     = 2,   // No local writes (cloud_only mode)
    };

    // Call after existing initStorage() — detects SD, selects mode.
    // Reads "storage.cloud_only" from platform_config.json if available.
    static bool begin(const char* cfgPath = "/platform_config.json");

    // Primary filesystem for sensor logs (nullptr in CLOUD mode)
    static fs::FS* primary();

    // Secondary (cache) filesystem — LittleFS when SD is primary, nullptr otherwise
    static fs::FS* secondary();

    static Mode    mode()    { return _mode; }
    static bool    sdReady() { return _sdOk; }

    // Write a buffer to both primary and secondary (best-effort mirror)
    static void mirrorWrite(const char* path, const uint8_t* data, size_t len);

    // Ensure directory exists on given filesystem
    static void ensureDir(fs::FS& fs, const char* dir);

private:
    static Mode   _mode;
    static bool   _sdOk;
    static uint8_t _sdCS;
};
