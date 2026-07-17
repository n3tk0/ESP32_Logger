#include "DataLogger.h"

#if PLATFORM_LEGACY_BUILD

#include "../core/Globals.h"
#include "../utils/AtomicWrite.h"
#include "../utils/MutexGuard.h"
#include "../pipeline/DataPipeline.h"
#include "StorageManager.h"
#include "RtcManager.h"
#include <LittleFS.h>
#include <math.h>

// Count newlines in a file (= number of log entries)
static int countFileLines(fs::FS* fs, const String& path) {
    File f = fs->open(path, "r");
    if (!f) return 0;
    int count = 0;
    while (f.available()) {
        if (f.read() == '\n') count++;
    }
    f.close();
    return count;
}

// Trim oldest entries from the file to stay within maxEntries limit
static bool trimLogFile(fs::FS* fs, const String& path, int maxEntries, int currentLines, int newEntries) {
    int totalAfterAppend = currentLines + newEntries;
    if (maxEntries <= 0 || totalAfterAppend <= maxEntries) return true;

    int linesToSkip = totalAfterAppend - maxEntries;
    if (linesToSkip <= 0) return true;

    // Caller (flushLogBufferToFS) already holds fsMutex — pass nullptr to
    // avoid self-deadlock on the non-recursive mutex.
    bool ok = atomicWrite(*fs, path.c_str(), [&](File& dst) -> bool {
        File src = fs->open(path, "r");
        if (!src) return false;
        int skipped = 0;
        while (src.available() && skipped < linesToSkip) {
            char c = src.read();
            if (c == '\n') skipped++;
        }
        uint8_t buf[256];
        while (src.available()) {
            size_t n = src.read(buf, sizeof(buf));
            if (n > 0 && dst.write(buf, n) != n) { src.close(); return false; }
        }
        src.close();
        return true;
    }, nullptr);
    if (ok) DBGF("Trimmed %d old entries from %s\n", linesToSkip, path.c_str());
    return ok;
}

void flushLogBufferToFS() {
    if (logBufferCount == 0 || !fsAvailable || !activeFS) return;

    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return;  // mutex exists but timed out — skip this flush

    String logFile = getActiveDatalogFile();

    // Create folder if needed
    if (strlen(config.datalog.folder) > 0) {
        String folder = String(config.datalog.folder);
        if (!folder.startsWith("/")) folder = "/" + folder;
        if (!activeFS->exists(folder)) activeFS->mkdir(folder);
    }

    // Enforce maxEntries: trim oldest lines before appending new ones
    if (config.datalog.maxEntries > 0) {
        int existingLines = countFileLines(activeFS, logFile);
        trimLogFile(activeFS, logFile, config.datalog.maxEntries, existingLines, logBufferCount);
    }

    File f = activeFS->open(logFile, FILE_APPEND);
    if (!f) { Serial.println("ERR: Can't open datalog"); return; }

    int writtenCount = 0;
    for (int i = 0; i < logBufferCount; i++) {
        // Convert UTC epochs to local time for display in log entries.
        struct tm wakeTm = {0}, sleepTm = {0};
        {
            time_t wt = (time_t)logBuffer[i].wakeTimestamp;
            time_t st = (time_t)logBuffer[i].sleepTimestamp;
            if (wt > 0) localtime_r(&wt, &wakeTm);
            if (st > 0) localtime_r(&st, &sleepTm);
        }

        String line;
        line.reserve(120);

        // Date
        if (config.datalog.dateFormat != DATE_OFF) {
            char dateBuf[12];
            switch (config.datalog.dateFormat) {
                case DATE_DDMMYYYY:
                    snprintf(dateBuf, 12, "%02d/%02d/%04d", wakeTm.tm_mday, wakeTm.tm_mon + 1, wakeTm.tm_year + 1900); break;
                case DATE_MMDDYYYY:
                    snprintf(dateBuf, 12, "%02d/%02d/%04d", wakeTm.tm_mon + 1, wakeTm.tm_mday, wakeTm.tm_year + 1900); break;
                case DATE_YYYYMMDD:
                    snprintf(dateBuf, 12, "%04d-%02d-%02d", wakeTm.tm_year + 1900, wakeTm.tm_mon + 1, wakeTm.tm_mday); break;
                case DATE_DDMMYYYY_DOT:
                    snprintf(dateBuf, 12, "%02d.%02d.%04d", wakeTm.tm_mday, wakeTm.tm_mon + 1, wakeTm.tm_year + 1900); break;
                default: dateBuf[0] = 0;
            }
            line += dateBuf;
        }

        // Start time
        char timeBuf[12];
        switch (config.datalog.timeFormat) {
            case TIME_HHMMSS:
                snprintf(timeBuf, 12, "%02d:%02d:%02d", wakeTm.tm_hour, wakeTm.tm_min, wakeTm.tm_sec); break;
            case TIME_HHMM:
                snprintf(timeBuf, 12, "%02d:%02d", wakeTm.tm_hour, wakeTm.tm_min); break;
            case TIME_12H: {
                int h = wakeTm.tm_hour % 12; if (!h) h = 12;
                snprintf(timeBuf, 12, "%d:%02d:%02d%s", h, wakeTm.tm_min, wakeTm.tm_sec,
                         wakeTm.tm_hour < 12 ? "AM" : "PM");
                break;
            }
        }
        if (line.length() > 0) line += "|";
        line += timeBuf;

        // End
        if (config.datalog.endFormat != END_OFF) {
            line += "|";
            if (config.datalog.endFormat == END_TIME) {
                switch (config.datalog.timeFormat) {
                    case TIME_HHMMSS:
                        snprintf(timeBuf, 12, "%02d:%02d:%02d", sleepTm.tm_hour, sleepTm.tm_min, sleepTm.tm_sec); break;
                    case TIME_HHMM:
                        snprintf(timeBuf, 12, "%02d:%02d", sleepTm.tm_hour, sleepTm.tm_min); break;
                    case TIME_12H: {
                        int h = sleepTm.tm_hour % 12; if (!h) h = 12;
                        snprintf(timeBuf, 12, "%d:%02d:%02d%s", h, sleepTm.tm_min, sleepTm.tm_sec,
                                 sleepTm.tm_hour < 12 ? "AM" : "PM");
                        break;
                    }
                }
                line += timeBuf;
            } else {
                uint32_t dur = logBuffer[i].sleepTimestamp - logBuffer[i].wakeTimestamp;
                line += String(dur) + "s";
            }
        }

        if (config.datalog.includeBootCount) { line += "|#:"; line += logBuffer[i].bootCount; }

        // Trigger
        line += "|"; line += logBuffer[i].wakeupReason;

        // Volume
        if (config.datalog.volumeFormat != VOL_OFF) {
            line += "|";
            String volStr = String(logBuffer[i].volumeLiters, 2);
            switch (config.datalog.volumeFormat) {
                case VOL_L_COMMA: volStr.replace('.', ','); line += "L:" + volStr; break;
                case VOL_L_DOT:   line += "L:" + volStr; break;
                default:          line += volStr;
            }
        }

        // Extra presses
        if (config.datalog.includeExtraPresses) {
            line += "|FF" + String(logBuffer[i].ffCount);
            line += "|PF" + String(logBuffer[i].pfCount);
        }

        // println appends CRLF; a short write means the FS is full/failing.
        // Stop at the first failure so we don't re-write already-persisted
        // lines on retry (which would duplicate entries in the log file).
        if (f.println(line) != line.length() + 2) break;
        writtenCount++;
    }

    f.close();
    int cnt = writtenCount;
    // Clear the buffer only if every entry made it to disk.  On a partial
    // write, shift the unwritten remainder to the front and keep it for the
    // next flush — so failed entries are retried without duplicating the ones
    // that already landed.
    if (writtenCount == logBufferCount) {
        logBufferCount = 0;
    } else {
        if (writtenCount > 0) {
            for (int i = writtenCount; i < logBufferCount; i++) {
                logBuffer[i - writtenCount] = logBuffer[i];
            }
            logBufferCount -= writtenCount;
        }
        Serial.println("ERR: datalog write failed — retaining remaining buffer for retry");
    }
    // backupBootCount() re-acquires fsMutex internally; release ours first so we
    // don't self-deadlock on the non-recursive mutex (H3 discipline).
    g.release();
    backupBootCount();
    DBGF("Flushed %d entries to %s\n", cnt, logFile.c_str());
}

void addLogEntry(uint32_t capturedPulses) {
    if (logBufferCount >= LOG_BATCH_SIZE) {
        flushLogBufferToFS();
        if (logBufferCount >= LOG_BATCH_SIZE) {
            for (int i = 0; i < LOG_BATCH_SIZE - 1; i++) logBuffer[i] = logBuffer[i + 1];
            logBufferCount = LOG_BATCH_SIZE - 1;
        }
    }

    int i = logBufferCount;
    logBuffer[i].wakeTimestamp = currentWakeTimestamp;

    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        logBuffer[i].sleepTimestamp = now.IsValid() ? now.Unix32Time() : 0;
    } else {
        logBuffer[i].sleepTimestamp = 0;
    }

    logBuffer[i].bootCount = (uint16_t)(bootCount & 0xFFFF);
    logBuffer[i].ffCount   = highCountFF;
    logBuffer[i].pfCount   = highCountPF;

    // L2: use pre-captured pulse count (caller clears pulseCount atomically)
    float ppl = config.flowMeter.pulsesPerLiter;
    if (ppl < 1.0f || !isfinite(ppl)) ppl = 450.0f;
    float cal = config.flowMeter.calibrationMultiplier;
    if (cal <= 0.0f || !isfinite(cal)) cal = 1.0f;
    logBuffer[i].volumeLiters = (float)capturedPulses / ppl * cal;

    String reason = onlineLoggerMode ? cycleStartedBy : wakeUpButtonStr;
    strncpy(logBuffer[i].wakeupReason, reason.c_str(), 9);
    logBuffer[i].wakeupReason[9] = '\0';

    logBufferCount++;
    highCountFF = 0;
    highCountPF = 0;
}

#else  // PLATFORM_LEGACY_BUILD == 0

// Legacy flowmeter run logger compiled out.  Stubs preserve link compatibility
// with Logger.ino call sites that are runtime-gated by g_platformMode.
void flushLogBufferToFS()                {}
void addLogEntry(uint32_t /*pulses*/)    {}

#endif  // PLATFORM_LEGACY_BUILD
