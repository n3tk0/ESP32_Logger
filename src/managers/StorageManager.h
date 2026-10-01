#pragma once
#include <Arduino.h>

// Forward declaration — the FS types are only referenced as pointers here, so
// <FS.h> belongs in StorageManager.cpp (header-bloat audit).
namespace fs { class FS; }

bool   initStorage();
fs::FS* getCurrentViewFS();

/// Where settings and state live: LittleFS whenever it is mounted, whatever
/// the storage type. With an SD card only the data log files (and the large
/// node-firmware / Kindle-package blobs) go to the card; platform_config.json,
/// alerts, the board profile, Kindle slots and the event log stay internal.
/// activeFS only when LittleFS is not available.
fs::FS* configFs();
void   getStorageInfo(uint64_t& used, uint64_t& total, int& percent,
                      const String& storageType = "");
String getActiveDatalogFile();
