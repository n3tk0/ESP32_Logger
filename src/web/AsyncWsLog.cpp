// src/web/AsyncWsLog.cpp — the web server library's error lines, rate-limited
// (src/compat/AsyncWebServerLoggingCustom.h).
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)

void asyncWsLogE(const char* fn, int line, const char* fmt, ...) {
    static uint32_t s_last = 0;
    static bool     s_any = false;
    static unsigned s_more = 0;     // lines held back since the last one
    const uint32_t now = millis();
    if (s_any && (int32_t)(now - s_last) < 10000) { s_more++; return; }
    // Claimed before the slow part: a second caller (the SSE push runs on
    // another task) arriving during Log.printf counts as held back.
    const unsigned held = s_more;
    s_last = now;
    s_any  = true;
    s_more = 0;
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (held) Log.printf("[async_ws] %s:%d %s (+%u held back)\n", fn, line, msg, held);
    else      Log.printf("[async_ws] %s:%d %s\n", fn, line, msg);
}
