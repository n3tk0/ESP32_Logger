#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../pipeline/LiveAggregator.h"
#include "../storage/JsonLogger.h"
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

// ---------------------------------------------------------------------------
// Legacy pipeline: per-reading JSONL writes.  Behaviour-identical to the
// pre-v13 implementation.
// ---------------------------------------------------------------------------
void runLegacyJsonl(const StorageTaskParam& p) {
    JsonLogger logger;
    JsonLogger mirrorLogger;
    bool       mirrorActive = false;

    if (p.fs) {
        logger.begin(*p.fs,
                     p.logDir    ? p.logDir    : "/logs",
                     p.maxSizeKB > 0 ? p.maxSizeKB : 512,
                     p.rotateDaily);
        if (p.mirrorFS) {
            mirrorLogger.begin(*p.mirrorFS,
                               p.logDir    ? p.logDir    : "/logs",
                               p.maxSizeKB > 0 ? p.maxSizeKB : 512,
                               p.rotateDaily);
            mirrorActive = true;
            Serial.println("[StorageTask] Mirror write active (jsonl)");
        }
    } else {
        Serial.println("[StorageTask] No filesystem — storage disabled");
    }

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();
        if (xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);
            logger.write(r);
            if (mirrorActive) mirrorLogger.write(r);
            if (fsMutex) xSemaphoreGive(fsMutex);
        }
    }
    if (fsMutex) xSemaphoreTake(fsMutex, portMAX_DELAY);
    logger.flush();
    if (mirrorActive) mirrorLogger.flush();
    if (fsMutex) xSemaphoreGive(fsMutex);
}

// ---------------------------------------------------------------------------
// Wide-CSV pipeline: aggregate in RAM, flush a single wide row per interval.
//
// One LiveAggregator instance feeds one or two CsvLogger writers (mirror
// support).  The aggregator owns its own mutex; fsMutex still serialises
// filesystem access against ConfigManager / DataLogger / web handlers.
// ---------------------------------------------------------------------------
void runWideCsv(const StorageTaskParam& p) {
    LiveAggregator agg;
    agg.setIntervalSec(p.aggregationIntervalSec);
    agg.setHumidityCorrection(p.humidityCorrectionEnabled,
                              p.humidityCorrectionKappa);

    CsvLogger primary;
    CsvLogger mirror;
    bool      mirrorActive = false;

    if (p.fs) {
        primary.begin(*p.fs,
                      p.logDir    ? p.logDir    : "/logs",
                      p.maxSizeKB > 0 ? p.maxSizeKB : 1024);
        if (p.mirrorFS) {
            mirror.begin(*p.mirrorFS,
                         p.logDir    ? p.logDir    : "/logs",
                         p.maxSizeKB > 0 ? p.maxSizeKB : 1024);
            mirrorActive = true;
            Serial.println("[StorageTask] Mirror write active (csv)");
        }
    } else {
        Serial.println("[StorageTask] No filesystem — storage disabled");
    }

    Serial.printf("[StorageTask] wide-CSV pipeline: interval=%us humCorr=%d kappa=%.2f\n",
                  (unsigned)agg.intervalSec(),
                  agg.humidityCorrection() ? 1 : 0,
                  agg.humidityKappa());

    char headerBuf[320];
    char rowBuf[320];

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();

        // Drain available readings into the aggregator (non-blocking).  The
        // 100 ms wait keeps the heartbeat fresh while still draining bursts.
        while (xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            agg.feed(r);
        }

        // Time-driven flush.
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

}  // namespace

// ---------------------------------------------------------------------------
void storageTaskFunc(void* param) {
    Serial.println("[StorageTask] started");
    auto* p = static_cast<StorageTaskParam*>(param);

    if (p && p->csvLoggingEnabled) {
        runWideCsv(*p);
    } else {
        runLegacyJsonl(p ? *p : StorageTaskParam{});
    }

    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
