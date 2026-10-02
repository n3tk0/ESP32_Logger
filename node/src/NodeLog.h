// ============================================================================
// node/src/NodeLog.h — serial log lines whose text stays in flash
//
// On the ESP8266 every string literal lives in RAM (.rodata is mapped into
// the 80 KB of DRAM, not read from flash), so a firmware that explains itself
// on the serial line — and this one does, at length, because a node on a wall
// has nothing else to say what went wrong — pays for each sentence in heap.
// PSTR/F keep the format in flash and printf_P reads it from there.
//
// Every line also goes into a small ring in RAM (NODE_LOG_RING bytes), which
// GET /api/log serves: the serial line is the one place the node says why,
// and nobody has a cable on a node that is on the wall.
// ============================================================================
#pragma once

#include <Arduino.h>

#ifndef NODE_LOG_RING
#  define NODE_LOG_RING 1536
#endif

/// printf_P to Serial and into the ring.
void nodeLogP(PGM_P fmt, ...) __attribute__((format(printf, 1, 2)));

/// Write the ring, oldest first, through `chunk` (at most two calls: the ring
/// wraps once). Nothing is copied.
void nodeLogEach(void (*chunk)(const char* p, size_t n, void* ctx), void* ctx);

/// Bytes currently held.
size_t nodeLogSize();

#define LOGF(fmt, ...) nodeLogP(PSTR(fmt), ##__VA_ARGS__)
#define LOGLN(s)       nodeLogP(PSTR(s "\n"))
