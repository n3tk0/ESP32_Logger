#include "EventLog.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <stdio.h>

#include "Globals.h"                   // activeFS, littleFsAvailable
#include "../pipeline/DataPipeline.h"  // fsMutex
#include "../utils/MutexGuard.h"

const char* const EVENT_LOG_PATH        = "/error_log.txt";
const char* const EVENT_LOG_LEGACY_PATH = "/reset_log.txt";

/// Which filesystem the log lives on.
///
/// activeFS when there is one, because that is what /api/diag reads back and a
/// log written where the reader does not look is not a log. LittleFS before
/// StorageManager has run, so a line written very early in boot — an OTA event,
/// say — still lands somewhere rather than being dropped.
static fs::FS* logFs() {
    if (activeFS) return activeFS;
    if (littleFsAvailable) return &LittleFS;
    return nullptr;
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
        Serial.printf("[log] %s and %s both exist — leaving the old one in place\n",
                      EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH);
        return;
    }

    if (fs->rename(EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH))
        Serial.printf("[log] renamed %s to %s\n",
                      EVENT_LOG_LEGACY_PATH, EVENT_LOG_PATH);
    else
        Serial.printf("[log] could not rename %s to %s\n",
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

    File f = fs->open(EVENT_LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.print(line);
    f.close();
}
