#include "StorageTask.h"
#include "TaskManager.h"
#include "../pipeline/DataPipeline.h"
#include "../pipeline/LiveAggregator.h"
#include "../pipeline/FlowRunLogger.h"
#include "../storage/Datalog.h"
#include "../core/Globals.h"   // bootCount, littleFsAvailable
#include "../utils/MutexGuard.h"
#include "../core/HeapWatch.h"   // dataLost
#include <LittleFS.h>
#include <string.h>

namespace {

// Best-effort current epoch — the pipeline's shared one; see
// pipelineNowEpoch() in TaskManager.h for why every task asks the same clock.
inline uint32_t nowEpochSafe() { return pipelineNowEpoch(); }

constexpr size_t HDR_BYTES  = DL_HEADER_MAX;   // > the widest header
constexpr size_t ROW_BYTES  = 512;    // base fields + DL_MAX_COLS values
constexpr size_t PEND_BYTES = 2048;   // DL_BATCH_ROWS rows of ~250 B; longer rows flush sooner

// The batch of rows not yet written, with the header they were formatted
// under: a row goes to a file with the same header, so a new header writes
// the batch first.
struct Batch {
    fs::FS*  fs      = nullptr;
    fs::FS*  mirror  = nullptr;
    char*    buf     = nullptr;      // PEND_BYTES, CRLF-terminated rows
    char*    hdr     = nullptr;      // HDR_BYTES
    char*    tmpHdr  = nullptr;      // HDR_BYTES
    char*    row     = nullptr;      // ROW_BYTES
    size_t   len     = 0;
    int      rows    = 0;
    uint32_t epoch   = 0;            // newest row
    uint32_t first   = 0;            // oldest row: the batch's period

    bool alloc() {
        char* m = (char*)malloc(PEND_BYTES + 2 * HDR_BYTES + ROW_BYTES);
        if (!m) return false;
        buf = m; hdr = m + PEND_BYTES; tmpHdr = hdr + HDR_BYTES; row = tmpHdr + HDR_BYTES;
        buf[0] = hdr[0] = '\0';
        return true;
    }

    // A ROW LOST TO THE MUTEX IS STILL A ROW LOST: counted in g_queueDrops
    // and said on the console, like every other way a row can disappear.
    void flush() {
        if (!rows || !fs) return;
        {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (fsMutex && !g.isLocked()) {
                Serial.printf("[StorageTask] %d row(s) LOST (fsMutex timeout)\n", rows);
                dataLost("datalog fsMutex timeout", (uint32_t)rows);
            } else {
                int w = datalogAppend(*fs, hdr, buf, rows, epoch);
                if (w < rows) {
                    Serial.printf("[StorageTask] %d of %d row(s) LOST\n", rows - (w > 0 ? w : 0), rows);
                    dataLost("datalog write", (uint32_t)(rows - (w > 0 ? w : 0)));
                }
                if (littleFsAvailable) datalogColsSaveIfLearned(LittleFS);
            }
        }
        // Released between the two so a slow SD write does not hold the
        // mutex for the whole dual write. (AUDIT 2.16)
        if (mirror) {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            const int w = (!fsMutex || g.isLocked())
                        ? datalogAppend(*mirror, hdr, buf, rows, epoch) : -1;
            if (w < rows) {
                Serial.printf("[StorageTask] mirror: %d of %d row(s) LOST\n", rows - (w > 0 ? w : 0), rows);
                dataLost("datalog mirror write", (uint32_t)(rows - (w > 0 ? w : 0)));
            }
        }
        len = 0; rows = 0; buf[0] = '\0';
    }

    // `wantRev` (0 = any): the columns' revision the values were combined
    // under. A header built from another — the columns or a mode changed
    // since — would label the values as what they are not; the row is
    // dropped, as the window it came from is.
    void add(const DatalogRow& r, const float* vals, int nVals, uint32_t wantRev = 0) {
        if (!buf) return;
        uint32_t rev = 0;
        if (datalogHeader(tmpHdr, HDR_BYTES, &rev) < 0) {
            Serial.println("[StorageTask] row dropped - header did not fit");
            dataLost("datalog header too long");
            return;
        }
        if (wantRev && rev != wantRev) return;
        const uint32_t t = r.end ? r.end : r.start;
        // A batch goes to one file: a new header or a new rotation period
        // writes what is pending first.
        if (rows && (strcmp(tmpHdr, hdr) != 0 || !datalogSamePeriod(first, t))) flush();
        const int n = dlFormatRow(row, ROW_BYTES, datalogLayout(), r, vals, nVals);
        if (n < 0) {
            Serial.println("[StorageTask] row dropped - did not fit");
            dataLost("datalog row too long");
            return;
        }
        if (len + n + 3 > PEND_BYTES) flush();
        if (!rows) { strlcpy(hdr, tmpHdr, HDR_BYTES); first = t; }
        memcpy(buf + len, row, n);
        len += n;
        buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = '\0';
        rows++;
        epoch = t;
        if (rows >= DL_BATCH_ROWS) flush();
    }
};

// The window just closed, each column by its own DatalogAgg, as a TIMER row.
void addSensorRow(Batch& b, const float* vals, uint32_t start, uint32_t end, uint32_t rev) {
    if (!datalogSensorRows()) return;   // the header carries no sensor columns
    DatalogRow r = {};
    r.start  = start;
    r.end    = end;
    r.boot   = (uint16_t)(bootCount & 0xFFFF);
    r.volume = NAN;
    r.ff = r.pf = -1;
    strlcpy(r.trigger, DL_TRIGGER_TIMER, sizeof(r.trigger));
    b.add(r, vals, datalogColsCopy(nullptr, DL_MAX_COLS), rev);
}

}  // namespace

// ---------------------------------------------------------------------------
void storageTaskFunc(void* param) {
    Serial.println("[StorageTask] started");

    // See SensorTask: park until init() opens the start gate before touching
    // the filesystem, so a shutdown that races startup exits before doing work.
    if (!TaskManager::waitForStart()) { Serial.println("[StorageTask] stopped"); vTaskDelete(nullptr); return; }

    auto* p = static_cast<StorageTaskParam*>(param);
    StorageTaskParam cfg = p ? *p : StorageTaskParam{};

    LiveAggregator agg;
    agg.setIntervalSec(cfg.aggregationIntervalSec);
    agg.setHumidityCorrection(cfg.humidityCorrectionEnabled,
                              cfg.humidityCorrectionKappa);

    Batch batch;
    batch.fs     = cfg.fs;
    batch.mirror = cfg.mirrorFS;
    if (cfg.fs && !batch.alloc()) {
        Serial.println("[StorageTask] no memory for the row batch - nothing will be logged");
        batch.fs = nullptr;
    }

    FlowRunLogger flowRunLog;
    bool          flowRunActive = cfg.enableFlowRunLogger && (batch.fs != nullptr);
    if (flowRunActive) {
        flowRunLog.setIdleTimeoutSec(cfg.flowRunIdleTimeoutSec);
        flowRunLog.setStartThreshold(cfg.flowRunStartThreshold);
    }

    uint32_t colsRev = datalogColsRev();
    float    vals[LiveAggregator::MAX_COLUMNS];
    bool     writing = false;

    Serial.printf("[StorageTask] interval=%us humCorr=%d kappa=%.2f sensors=%d runLog=%d\n",
                  (unsigned)agg.intervalSec(),
                  agg.humidityCorrection() ? 1 : 0,
                  agg.humidityKappa(),
                  (cfg.csvLoggingEnabled && batch.fs) ? 1 : 0,
                  flowRunActive ? 1 : 0);

    SensorReading r;
    while (TaskManager::running) {
        g_taskHeartbeat[TASK_IDX_STORAGE] = millis();

        // Re-read live config knobs from *p so a settings save or a
        // /api/config/platform reload applies without a task restart.
        // (AUDIT 11.5)
        if (p) {
            agg.setIntervalSec(p->aggregationIntervalSec
                                   ? p->aggregationIntervalSec : 60);
            agg.setHumidityCorrection(p->humidityCorrectionEnabled,
                                       p->humidityCorrectionKappa > 0.0f
                                           ? p->humidityCorrectionKappa : 0.35f);
            const bool want = p->csvLoggingEnabled && batch.fs != nullptr;
            if (want != writing) agg.reset();
            writing = want;
        }
        // Drain available readings. R14 / AUDIT 11.6: at most 32 per outer
        // iteration so the heartbeat above is refreshed every 3.2 s even
        // under a sustained burst.
        int drained = 0;
        while (drained++ < 32 &&
               xQueueReceive(storageQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (writing) agg.feed(r, datalogColsLearn(r.sensorId, r.metric));
            // The reading's own timestamp for the run's duration; now only
            // when it has none. (AUDIT 11.7)
            if (flowRunActive)
                flowRunLog.feed(r, r.timestamp > 0 ? r.timestamp : nowEpochSafe());
        }

        const uint32_t epoch = nowEpochSafe();
        if (flowRunActive) {
            flowRunLog.tick(epoch);
            DatalogRow run;
            if (flowRunLog.takeRun(run)) {
                run.boot = (uint16_t)(bootCount & 0xFFFF);
                batch.add(run, nullptr, 0);
                batch.flush();   // a fill is rare and worth not losing
            }
        }

        // Each column's mode, with the revision it belongs to, read once
        // after the queue is drained. A changed revision — a column
        // renumbered or its mode changed while the readings went in — means
        // the window's sums belong to another list: it is dropped, and the
        // next one starts under the new list. Otherwise the window closes,
        // when it is due, each column by its own mode.
        uint32_t start = 0;
        const bool flushReq = datalogFlushRequested();
        uint8_t  modes[LiveAggregator::MAX_COLUMNS];
        uint32_t rev = 0;
        const int nModes = datalogColsAggs(modes, LiveAggregator::MAX_COLUMNS, &rev);
        if (rev != colsRev) {
            agg.reset();
            colsRev = rev;
        } else if (writing && agg.take(epoch, flushReq, vals, &start, modes, nModes)) {
            addSensorRow(batch, vals, start, epoch, colsRev);
        }
        if (flushReq) {
            batch.flush();
            datalogFlushDone();
        }
    }

    // Exit (deep sleep, shutdown): the window in progress and the batch.
    uint32_t start = 0;
    const uint32_t epoch = nowEpochSafe();
    if (writing) {
        uint8_t  modes[LiveAggregator::MAX_COLUMNS];
        uint32_t rev = 0;
        const int nModes = datalogColsAggs(modes, LiveAggregator::MAX_COLUMNS, &rev);
        if (rev == colsRev && agg.take(epoch, true, vals, &start, modes, nModes))
            addSensorRow(batch, vals, start, epoch, colsRev);
    }
    batch.flush();
    datalogFlushDone();

    Serial.println("[StorageTask] stopped");
    vTaskDelete(nullptr);
}
