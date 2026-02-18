#pragma once
#include <Arduino.h>
#include <FS.h>

bool   initStorage();
fs::FS* getCurrentViewFS();
void   getStorageInfo(uint64_t& used, uint64_t& total, int& percent,
                      const String& storageType = "");
String getStorageBarColor(int percent);
String generateDatalogFileOptions();
int    countDatalogFiles();
String getActiveDatalogFile();
