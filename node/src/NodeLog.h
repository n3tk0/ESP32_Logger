// ============================================================================
// node/src/NodeLog.h — serial log lines whose text stays in flash
//
// On the ESP8266 every string literal lives in RAM (.rodata is mapped into
// the 80 KB of DRAM, not read from flash), so a firmware that explains itself
// on the serial line — and this one does, at length, because a node on a wall
// has nothing else to say what went wrong — pays for each sentence in heap.
// PSTR/F keep the format in flash and printf_P reads it from there.
// ============================================================================
#pragma once

#include <Arduino.h>

#define LOGF(fmt, ...) Serial.printf_P(PSTR(fmt), ##__VA_ARGS__)
#define LOGLN(s)       Serial.println(F(s))
