#include "DataLogger.h"
#include "../core/Globals.h"
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
static void trimLogFile(fs::FS* fs, const String& path, int maxEntries, int currentLines, int newEntries) {
    int totalAfterAppend = currentLines + newEntries;
    if (maxEntries <= 0 || totalAfterAppend <= maxEntries) return;

    int linesToSkip = totalAfterAppend - maxEntries;
    if (linesToSkip <= 0) return;

    String tmpPath = path + ".tmp";
    File src = fs->open(path, "r");
    if (!src) return;
    File dst = fs->open(tmpPath, "w");
    if (!dst) { src.close(); return; }

    int skipped = 0;
    while (src.available() && skipped < linesToSkip) {
        char c = src.read();
        if (c == '\n') skipped++;
    }
    // Copy remaining lines
    uint8_t buf[256];
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        if (n > 0) dst.write(buf, n);
    }
    src.close();
    dst.close();
    fs->remove(path);
    fs->rename(tmpPath, path);
    DBGF("Trimmed %d old entries from %s\n", linesToSkip, path.c_str());
}

void flushLogBufferToFS() {
    if (logBufferCount == 0 || !fsAvailable || !activeFS) return;

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
    if (!f) { DBGLN("ERR: Can't open datalog"); return; }

    for (int i = 0; i < logBufferCount; i++) {
        RtcDateTime wakeTime, sleepTime;
        wakeTime.InitWithUnix32Time(logBuffer[i].wakeTimestamp);
        sleepTime.InitWithUnix32Time(logBuffer[i].sleepTimestamp);

        String line;
        line.reserve(120);

        // Date
        if (config.datalog.dateFormat != DATE_OFF) {
            char dateBuf[12];
            switch (config.datalog.dateFormat) {
                case DATE_DDMMYYYY:
                    snprintf(dateBuf, 12, "%02u/%02u/%04u", wakeTime.Day(), wakeTime.Month(), wakeTime.Year()); break;
                case DATE_MMDDYYYY:
                    snprintf(dateBuf, 12, "%02u/%02u/%04u", wakeTime.Month(), wakeTime.Day(), wakeTime.Year()); break;
                case DATE_YYYYMMDD:
                    snprintf(dateBuf, 12, "%04u-%02u-%02u", wakeTime.Year(), wakeTime.Month(), wakeTime.Day()); break;
                case DATE_DDMMYYYY_DOT:
                    snprintf(dateBuf, 12, "%02u.%02u.%04u", wakeTime.Day(), wakeTime.Month(), wakeTime.Year()); break;
                default: dateBuf[0] = 0;
            }
            line += dateBuf;
        }

        // Start time
        char timeBuf[12];
        switch (config.datalog.timeFormat) {
            case TIME_HHMMSS:
                snprintf(timeBuf, 12, "%02u:%02u:%02u", wakeTime.Hour(), wakeTime.Minute(), wakeTime.Second()); break;
            case TIME_HHMM:
                snprintf(timeBuf, 12, "%02u:%02u", wakeTime.Hour(), wakeTime.Minute()); break;
            case TIME_12H: {
                uint8_t h = wakeTime.Hour() % 12; if (!h) h = 12;
                snprintf(timeBuf, 12, "%u:%02u:%02u%s", h, wakeTime.Minute(), wakeTime.Second(),
                         wakeTime.Hour() < 12 ? "AM" : "PM");
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
                        snprintf(timeBuf, 12, "%02u:%02u:%02u", sleepTime.Hour(), sleepTime.Minute(), sleepTime.Second()); break;
                    case TIME_HHMM:
                        snprintf(timeBuf, 12, "%02u:%02u", sleepTime.Hour(), sleepTime.Minute()); break;
                    case TIME_12H: {
                        uint8_t h = sleepTime.Hour() % 12; if (!h) h = 12;
                        snprintf(timeBuf, 12, "%u:%02u:%02u%s", h, sleepTime.Minute(), sleepTime.Second(),
                                 sleepTime.Hour() < 12 ? "AM" : "PM");
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

        f.println(line);
    }

    f.close();
    int cnt = logBufferCount;
    logBufferCount = 0;
    backupBootCount();
    DBGF("Flushed %d entries to %s\n", cnt, logFile.c_str());
}

void addLogEntry() {
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

    noInterrupts();
    uint32_t safePulse = pulseCount;
    pulseCount = 0;
    interrupts();

    float ppl = config.flowMeter.pulsesPerLiter;
    if (ppl < 1.0f || !isfinite(ppl)) ppl = 450.0f;
    float cal = config.flowMeter.calibrationMultiplier;
    if (cal <= 0.0f || !isfinite(cal)) cal = 1.0f;
    logBuffer[i].volumeLiters = (float)safePulse / ppl * cal;

    String reason = onlineLoggerMode ? cycleStartedBy : wakeUpButtonStr;
    strncpy(logBuffer[i].wakeupReason, reason.c_str(), 9);
    logBuffer[i].wakeupReason[9] = '\0';

    logBufferCount++;
    highCountFF = 0;
    highCountPF = 0;
}
