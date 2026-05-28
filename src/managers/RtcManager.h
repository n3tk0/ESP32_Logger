#pragma once
#include <Arduino.h>

void   initRtc();
void   backupBootCount();
void   restoreBootCount();
String getRtcDateTimeString();
void   configureWakeup();
String getWakeupReason();
