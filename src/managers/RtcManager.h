#pragma once
#include <Arduino.h>

void   initRtc();
void   backupBootCount();
void   restoreBootCount();
String getRtcDateTimeString();
/// Set TZ from config.network (zone + daylight-saving rule) for every
/// localtime_r() that follows. Call after the config loads or changes.
void   applyTimeZone();
/// The same TZ string, for configTzTime().
void   currentPosixTz(char* out, size_t cap);
void   configureWakeup();
String getWakeupReason();
