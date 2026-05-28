#pragma once
#include <Arduino.h>

// Forward declaration — the FS types are only referenced as pointers here, so
// <FS.h> belongs in StorageManager.cpp (header-bloat audit).
namespace fs { class FS; }

bool   initStorage();
fs::FS* getCurrentViewFS();
void   getStorageInfo(uint64_t& used, uint64_t& total, int& percent,
                      const String& storageType = "");
String getActiveDatalogFile();
