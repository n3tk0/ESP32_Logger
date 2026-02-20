#pragma once
#include <Arduino.h>

void   initRtc();
void   backupBootCount();
void   restoreBootCount();
String getRtcTimeString();
String getRtcDateTimeString();
void   configureWakeup();
String getWakeupReason();
