#include "EventLog.h"
#include "LogRing.h"   // Log: Serial + the RTC log ring (/api/log)

#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Globals.h"                   // activeFS, littleFsAvailable
#include "../pipeline/DataPipeline.h"  // fsMutex
#include "../utils/MutexGuard.h"

const char* const EVENT_LOG_PATH        = "/error_log.txt";
const char* const EVENT_LOG_LEGACY_PATH = "/reset_log.txt";

/// Which filesystem the log lives on.
///
/// LittleFS, like every other setting and state file (configFs() in
/// StorageManager): /api/diag reads it back from there, and with an SD card
/// only the data log belongs on the card. activeFS only when LittleFS is not
/// mounted.
static fs::FS* logFs() {
    if (littleFsAvailable) return &LittleFS;
    return activeFS;
}

void eventLogMigrate() {
    fs::FS* fs = logFs();
    if (!fs) return;
    if (!fs->exists(EVENT_LOG_LEGACY_PATH)) return;

    if (fs->exists(EVENT_LOG_PATH)) {
        // Both present. This does not happen in a normal upgrade — the new name
        // is only ever created by firmware that migrates first — so rather than
        // guess at merge order, say so and leave both files intact. Nothing is
        // lost, and the old one is still downloadable from the Files page.
        Log.printf("[log] %s and %s both exist — leaving the old one in place\n",
                      EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH);
        return;
    }

    if (fs->rename(EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH))
        Log.printf("[log] renamed %s to %s\n",
                      EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH);
    else
        Log.printf("[log] could not rename %s to %s\n",
                      EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH);
}

void eventLogPrintf(const char* fmt, ...) {
    fs::FS* fs = logFs();
    if (!fs || !fmt) return;

    // Formatted BEFORE the mutex is taken. vsnprintf into 160 bytes of stack is
    // not slow, but it is not zero either, and there is no reason for it to
    // happen while another task is waiting to write a datalog row.
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 2;
    line[len++] = '\n';
    line[len]   = '\0';

    // The mutex is checked for existence, not just for acquisition. The two
    // earliest writers — the reset-reason line and the first OTA event — run in
    // setup() before TaskManager has created fsMutex, and treating "there is no
    // lock" as "I could not take the lock" would have dropped exactly the lines
    // written when the device has just come back from a crash. There are no
    // other tasks yet at that point, so there is nothing to serialise against.
    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return;

    // Bounded: /api/diag reads the file whole (and gives up past 8 KB), and
    // every boot adds a line. Past EVENT_LOG_MAX the newest EVENT_LOG_KEEP
    // bytes, from a line start, are kept and the rest goes.
    constexpr size_t EVENT_LOG_MAX  = 6 * 1024;
    constexpr size_t EVENT_LOG_KEEP = 3 * 1024;
    if (File old = fs->open(EVENT_LOG_PATH, FILE_READ)) {
        const size_t size = old.size();
        if (size + len > EVENT_LOG_MAX) {
            char* tail = (char*)malloc(EVENT_LOG_KEEP);
            size_t n = 0;
            if (tail) {
                old.seek(size - EVENT_LOG_KEEP);
                n = old.read((uint8_t*)tail, EVENT_LOG_KEEP);
            }
            old.close();
            if (tail) {
                const char* nl  = (const char*)memchr(tail, '\n', n);
                const size_t at = nl ? (size_t)(nl - tail) + 1 : n;
                if (File f = fs->open(EVENT_LOG_PATH, FILE_WRITE)) {
                    f.write((const uint8_t*)tail + at, n - at);
                    f.print(line);
                    f.close();
                }
                free(tail);
                return;
            }
            // No heap for the tail: append anyway, a long log beats a lost line.
        } else {
            old.close();
        }
    }

    File f = fs->open(EVENT_LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.print(line);
    f.close();
}

void eventLogNow(char* out, size_t cap) {
    if (!out || cap == 0) return;
    const time_t now = time(nullptr);
    struct tm t;
    if (now < 1600000000 || !localtime_r(&now, &t) ||
        strftime(out, cap, "%Y-%m-%d %H:%M:%S", &t) == 0)
        strlcpy(out, "?", cap);
}
