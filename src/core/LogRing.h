// ============================================================================
// src/core/LogRing.h — the serial log, kept where a crash does not erase it
//
// The serial line is where the firmware says what it is doing, and nobody has
// a cable on a Collector in a cupboard. Every log line goes through `Log`
// (a Print, so Log.printf / Log.println / Log.print work as Serial's do): it
// is written to Serial and into a ring in RTC memory, which GET /api/log
// serves.
//
// RTC memory is not cleared by a panic, a watchdog or a software reset, only
// by power-on, so after a crash /api/log still has the last lines written
// before it. At boot the tail of the previous boot's ring is set aside
// (LOG_RING_PREV bytes) before this boot's output overwrites it.
//
// What it cannot catch: the panic handler's own "Guru Meditation" report,
// which the ROM prints straight to the UART/USB, past this code.
//
// The ring costs no heap: on the C3 the RTC fast memory (8 KB) was all but
// unused (16 B).
// ============================================================================
#pragma once

#include <Arduino.h>

// The host tests' Arduino shim defines Log itself (a no-op sink like its
// Serial); everything below is the device's.
#ifndef LOG_RING_HOST_SHIM

#ifndef LOG_RING_BYTES
#  define LOG_RING_BYTES 4096
#endif
#ifndef LOG_RING_PREV
#  define LOG_RING_PREV  2048
#endif

class LogPrint : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;
    using Print::write;
};

/// Serial plus the RTC ring. Use it wherever Serial.printf/println/print was.
extern LogPrint Log;

/// Call first thing in setup(), before anything logs: sets the previous
/// boot's tail aside, or clears the ring after a power-on.
void logRingBegin();

/// What GET /api/log returns, fixed when the request arrives: the previous
/// boot's tail, then this boot's ring as it was at that moment, oldest first,
/// each under a heading line.
struct LogRingView {
    uint32_t liveStart;   // absolute position of this boot's oldest byte
    uint32_t liveLen;
    uint16_t prevLen;
    size_t   size;        // total bytes, headings included
};
LogRingView logRingView();

/// Copies up to `max` bytes of the view's text from `index` into `out` and
/// returns how many. Lines logged while the response is still being sent can
/// overwrite the oldest part of the ring; the response then shows that newer
/// text in its place (never more bytes than the view promised).
size_t logRingRead(const LogRingView& v, size_t index, uint8_t* out, size_t max);

#endif // LOG_RING_HOST_SHIM
