#include "DataLogger.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)

#if PLATFORM_LEGACY_BUILD

#include "../core/Globals.h"
#include "../storage/Datalog.h"
#include "../utils/MutexGuard.h"
#include "../pipeline/DataPipeline.h"
#include "StorageManager.h"
#include "RtcManager.h"
#include <math.h>
#include <memory>
#include <new>
#include <string.h>

// One row per wake, in the data log's shared format (storage/Datalog.h):
// the columns of the file are the same whichever writer adds a row, and
// datalogAppend() rotates, checks the header and trims.
void flushLogBufferToFS() {
    if (logBufferCount == 0 || !fsAvailable || !activeFS) return;

    // On the heap: this runs on the loop task's stack.
    constexpr size_t HDR_MAX = DL_HEADER_MAX;
    std::unique_ptr<char[]> header(new (std::nothrow) char[HDR_MAX]);
    if (!header || datalogHeader(header.get(), HDR_MAX) < 0) return;
    const DatalogLayout layout = datalogLayout();

    // CRLF-terminated rows, the line ending this log has always had.
    constexpr size_t ROW_MAX = 128;
    std::unique_ptr<char[]> lines(new (std::nothrow) char[LOG_BATCH_SIZE * ROW_MAX]);
    if (!lines) { Log.println("ERR: datalog - out of memory"); return; }
    size_t len = 0;
    int n = 0;
    for (int i = 0; i < logBufferCount; i++) {
        const LogEntry& e = logBuffer[i];
        DatalogRow r = {};
        r.start  = e.wakeTimestamp;
        r.end    = e.sleepTimestamp;
        r.boot   = e.bootCount;
        r.volume = e.volumeLiters;
        r.ff     = (int16_t)e.ffCount;
        r.pf     = (int16_t)e.pfCount;
        strlcpy(r.trigger, e.wakeupReason, sizeof(r.trigger));
        int w = dlFormatRow(lines.get() + len, ROW_MAX - 2, layout, r, nullptr, 0);
        if (w < 0) continue;
        len += w;
        lines[len++] = '\r'; lines[len++] = '\n'; lines[len] = '\0';
        n++;
    }

    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (fsMutex && !g.isLocked()) return;  // mutex exists but timed out — skip this flush

    const uint32_t newest = logBuffer[logBufferCount - 1].wakeTimestamp;
    int written = datalogAppend(*activeFS, header.get(), lines.get(), n, newest);
    if (written < 0) return;
    // Clear the buffer only if every entry made it to disk. On a partial
    // write, keep the unwritten remainder for the next flush — so failed
    // entries are retried without duplicating the ones that already landed.
    if (written >= logBufferCount) {
        logBufferCount = 0;
    } else {
        for (int i = written; i < logBufferCount; i++) logBuffer[i - written] = logBuffer[i];
        logBufferCount -= written;
        Log.println("ERR: datalog write failed — retaining remaining buffer for retry");
    }
    // backupBootCount() re-acquires fsMutex internally; release ours first so we
    // don't self-deadlock on the non-recursive mutex (H3 discipline).
    g.release();
    backupBootCount();
    DBGF("Flushed %d entries\n", written);
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
        MutexGuard rg(rtcMutex, pdMS_TO_TICKS(200));
        if (rtcMutex && !rg.isLocked()) {
            logBuffer[i].sleepTimestamp = 0;   // bus busy; 0 means "unknown"
        } else {
            RtcDateTime now = Rtc->GetDateTime();
            logBuffer[i].sleepTimestamp = now.IsValid() ? now.Unix32Time() : 0;
        }
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
