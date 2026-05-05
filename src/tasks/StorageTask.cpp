#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../pipeline/LiveAggregator.h"
#include "../storage/CsvLogger.h"
#include "../core/Globals.h"   // Rtc for epoch fallback
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
    bool      mirrorActive = false;
    bool      writingEnabled = cfg.csvLoggingEnabled && (cfg.fs != nullptr);

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

    Serial.printf("[StorageTask] interval=%us humCorr=%d kappa=%.2f writing=%d\n",
                  (unsigned)agg.intervalSec(),
                  agg.humidityCorrection() ? 1 : 0,
                  agg.humidityKappa(),
                  writingEnabled ? 1 : 0);

    char headerBuf[LiveAggregator::ROW_BUF_BYTES];
    char rowBuf   [LiveAggregator::ROW_BUF_BYTES];

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();

        // Drain available readings into the aggregator.  The 100 ms wait keeps
        // the heartbeat fresh while still draining bursts.
        while (xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            agg.feed(r);
        }

        if (!writingEnabled) continue;

        uint32_t epoch = nowEpochSafe();
        uint32_t rowTs = 0;
        if (agg.buildRowIfDue(epoch, rowBuf, sizeof(rowBuf), &rowTs)) {
            if (agg.buildHeader(headerBuf, sizeof(headerBuf)) > 0) {
                if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);
                primary.appendRow(rowTs, headerBuf, rowBuf);
                if (mirrorActive) mirror.appendRow(rowTs, headerBuf, rowBuf);
                if (fsMutex) xSemaphoreGive(fsMutex);
            }
        }
    }

    // Final flush on shutdown (best-effort).
    if (writingEnabled) {
        uint32_t rowTs = 0;
        if (agg.flushNow(nowEpochSafe(), rowBuf, sizeof(rowBuf), &rowTs)) {
            if (agg.buildHeader(headerBuf, sizeof(headerBuf)) > 0) {
                if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);
                primary.appendRow(rowTs, headerBuf, rowBuf);
                if (mirrorActive) mirror.appendRow(rowTs, headerBuf, rowBuf);
                if (fsMutex) xSemaphoreGive(fsMutex);
            }
        }
    }

    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
