#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../pipeline/LiveAggregator.h"
#include "../pipeline/FlowRunLogger.h"
#include "../storage/CsvLogger.h"
#include "../core/Globals.h"   // Rtc for epoch fallback
#include "../utils/MutexGuard.h"
#include <time.h>

namespace {

// ---------------------------------------------------------------------------
// Best-effort current epoch.  Tries the RTC first, then the system clock
// (set by NTP), and finally falls back to a millis-based monotonic counter
// so the aggregator still flushes on cadence even before time is known.
// ---------------------------------------------------------------------------
uint32_t nowEpochSafe() {
    if (Rtc) {
        RtcDateTime n = Rtc->GetDateTime();
        if (n.IsValid() && n.Year() >= 2020) return n.Unix32Time();
    }
    time_t sysT = time(nullptr);
    if (sysT > 1000000000) return (uint32_t)sysT;
    return (uint32_t)(millis() / 1000UL);
}

}  // namespace

// ---------------------------------------------------------------------------
void storageTaskFunc(void* param) {
    Serial.println("[StorageTask] started");
    auto* p = static_cast<StorageTaskParam*>(param);
    StorageTaskParam cfg = p ? *p : StorageTaskParam{};

    LiveAggregator agg;
    agg.setIntervalSec(cfg.aggregationIntervalSec);
    agg.setHumidityCorrection(cfg.humidityCorrectionEnabled,
                              cfg.humidityCorrectionKappa);

    CsvLogger primary;
    CsvLogger mirror;
    bool      mirrorActive  = false;
    bool      writingEnabled = cfg.csvLoggingEnabled && (cfg.fs != nullptr);

    FlowRunLogger flowRunLog;
    bool          flowRunActive = cfg.enableFlowRunLogger && (cfg.fs != nullptr);
    if (flowRunActive) {
        flowRunLog.setIdleTimeoutSec(cfg.flowRunIdleTimeoutSec);
        flowRunLog.setStartThreshold(cfg.flowRunStartThreshold);
        flowRunLog.begin(*cfg.fs,
                         cfg.flowRunLogDir ? cfg.flowRunLogDir : "/runs",
                         256);
    }

    if (writingEnabled) {
        primary.begin(*cfg.fs,
                      cfg.logDir    ? cfg.logDir    : "/logs",
                      cfg.maxSizeKB > 0 ? cfg.maxSizeKB : 1024);
        if (cfg.mirrorFS) {
            mirror.begin(*cfg.mirrorFS,
                         cfg.logDir    ? cfg.logDir    : "/logs",
                         cfg.maxSizeKB > 0 ? cfg.maxSizeKB : 1024);
            mirrorActive = true;
            Serial.println("[StorageTask] Mirror write active");
        }
    } else if (!cfg.csvLoggingEnabled) {
        Serial.println("[StorageTask] CSV logging disabled — drain-only mode");
    } else {
        Serial.println("[StorageTask] No filesystem — drain-only mode");
    }

    Serial.printf("[StorageTask] interval=%us humCorr=%d kappa=%.2f writing=%d runLog=%d\n",
                  (unsigned)agg.intervalSec(),
                  agg.humidityCorrection() ? 1 : 0,
                  agg.humidityKappa(),
                  writingEnabled ? 1 : 0,
                  flowRunActive  ? 1 : 0);

    char headerBuf[LiveAggregator::ROW_BUF_BYTES];
    char rowBuf   [LiveAggregator::ROW_BUF_BYTES];

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();

        // Re-read live config knobs from *p so a /api/config/platform reload
        // propagates without a task restart.  (AUDIT 11.5)
        if (p) {
            agg.setIntervalSec(p->aggregationIntervalSec
                                   ? p->aggregationIntervalSec : 60);
            agg.setHumidityCorrection(p->humidityCorrectionEnabled,
                                       p->humidityCorrectionKappa > 0.0f
                                           ? p->humidityCorrectionKappa : 0.35f);
            writingEnabled = p->csvLoggingEnabled && (p->fs != nullptr);
        }

        // Drain available readings into the aggregator (and the run logger
        // when active).  The 100 ms wait keeps the heartbeat fresh while
        // still draining bursts.  When CSV writing is disabled the
        // aggregator must be skipped — it would otherwise accumulate sum/
        // count forever (no flush ever resets them in drain-only mode).
        // R14 / AUDIT 11.6: cap inner drain at 32 readings per outer
        // iteration so the heartbeat refresh at line 85 happens at least
        // every 32 × 100 ms = 3.2 s worst-case. Under sustained sensor
        // burst the old unbounded loop could spin for the full 30-s
        // watchdog window without ever updating g_taskHeartbeat.
        int drained = 0;
        while (drained++ < 32 &&
               xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (writingEnabled) agg.feed(r);
            // Use the reading's own timestamp for FlowRunLogger duration
            // accounting; fall back to nowEpochSafe() only when ts is absent.
            // (AUDIT 11.7)
            if (flowRunActive) {
                uint32_t feedTs = (r.timestamp > 0) ? r.timestamp : nowEpochSafe();
                flowRunLog.feed(r, feedTs);
            }
        }

        uint32_t epoch = nowEpochSafe();
        if (flowRunActive) {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (g.isLocked()) flowRunLog.tick(epoch);
        }

        if (!writingEnabled) continue;

        uint32_t rowTs = 0;
        if (agg.buildRowIfDue(epoch, rowBuf, sizeof(rowBuf), &rowTs)) {
            if (agg.buildHeader(headerBuf, sizeof(headerBuf)) > 0) {
                // Release fsMutex between primary and mirror so a slow SD write
                // (50-100 ms) doesn't block the mutex for the full dual-write
                // window.  (AUDIT 2.16)
                {
                    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
                    if (g.isLocked()) primary.appendRow(rowTs, headerBuf, rowBuf);
                }
                if (mirrorActive) {
                    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
                    if (g.isLocked()) mirror.appendRow(rowTs, headerBuf, rowBuf);
                }
            }
        }
    }

    // Final flush on shutdown (best-effort).
    if (writingEnabled) {
        uint32_t rowTs = 0;
        if (agg.flushNow(nowEpochSafe(), rowBuf, sizeof(rowBuf), &rowTs)) {
            if (agg.buildHeader(headerBuf, sizeof(headerBuf)) > 0) {
                MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
                if (g.isLocked()) {
                    primary.appendRow(rowTs, headerBuf, rowBuf);
                    if (mirrorActive) mirror.appendRow(rowTs, headerBuf, rowBuf);
                }
            }
        }
    }

    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
