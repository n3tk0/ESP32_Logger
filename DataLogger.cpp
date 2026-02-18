#include "DataLogger.h"
#include "Globals.h"
#include "StorageManager.h"
#include "RtcManager.h"
#include <LittleFS.h>

void flushLogBufferToFS() {
    if (logBufferCount == 0 || !fsAvailable || !activeFS) return;

    String logFile = getActiveDatalogFile();

    // Create folder if needed
    if (strlen(config.datalog.folder) > 0) {
        String folder = String(config.datalog.folder);
        if (!folder.startsWith("/")) folder = "/" + folder;
        if (!activeFS->exists(folder)) activeFS->mkdir(folder);
    }

    File f = activeFS->open(logFile, FILE_APPEND);
    if (!f) { Serial.println("ERR: Can't open datalog"); return; }

    for (int i = 0; i < logBufferCount; i++) {
        RtcDateTime wakeTime, sleepTime;
        wakeTime.InitWithUnix32Time(logBuffer[i].wakeTimestamp);
        sleepTime.InitWithUnix32Time(logBuffer[i].sleepTimestamp);

        String line = "";

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

    logBuffer[i].bootCount = bootCount;
    logBuffer[i].ffCount   = highCountFF;
    logBuffer[i].pfCount   = highCountPF;

    noInterrupts();
    uint32_t safePulse = pulseCount;
    pulseCount = 0;
    interrupts();

    logBuffer[i].volumeLiters = (float)safePulse
                                / config.flowMeter.pulsesPerLiter
                                * config.flowMeter.calibrationMultiplier;

    String reason = onlineLoggerMode ? cycleStartedBy : wakeUpButtonStr;
    strncpy(logBuffer[i].wakeupReason, reason.c_str(), 9);
    logBuffer[i].wakeupReason[9] = '\0';

    logBufferCount++;
    highCountFF = 0;
    highCountPF = 0;
}
