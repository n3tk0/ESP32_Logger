#pragma once
// src/compat/AsyncWebServerLoggingCustom.h — ESPAsyncWebServer's log macros
// (picked up through -DASYNCWEBSERVER_LOG_CUSTOM, platformio.ini).
//
// Errors only, and at most one line per 10 s (src/web/AsyncWsLog.cpp). The
// library's own logging printed "Failed to allocate" once per write when a
// response buffer could not grow — once per byte under ArduinoJson — and the
// UART kept the AsyncTCP task busy past the 5 s task watchdog (2026-10-16).

void asyncWsLogE(const char* fn, int line, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define async_ws_log_e(format, ...) asyncWsLogE(__FUNCTION__, __LINE__, format, ##__VA_ARGS__)
#define async_ws_log_w(format, ...) do {} while (0)
#define async_ws_log_i(format, ...) do {} while (0)
#define async_ws_log_d(format, ...) do {} while (0)
#define async_ws_log_v(format, ...) do {} while (0)
