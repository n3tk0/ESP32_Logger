#pragma once
#include <Arduino.h>

void addLogEntry(uint32_t capturedPulses);
void flushLogBufferToFS();

