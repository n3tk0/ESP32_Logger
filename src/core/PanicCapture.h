#pragma once
// src/core/PanicCapture.h — see PanicCapture.cpp.

// A task whose place is written into the RTC log when the task watchdog
// fires (the AsyncTCP task: the one the watchdog watches besides IDLE).
// Takes a TaskHandle_t; nullptr is ignored.
void panicCaptureWatchTask(void* task);
