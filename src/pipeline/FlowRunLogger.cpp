#include "FlowRunLogger.h"
#include "../utils/MutexGuard.h"
#include "../pipeline/DataPipeline.h"
#include <math.h>
#include <string.h>

namespace {
class Lock {
public:
    Lock(SemaphoreHandle_t s, TickType_t t = pdMS_TO_TICKS(2000))
        : _s(s), _g(s, t) {}
    bool ok() const { return _s == nullptr || _g.isLocked(); }
private:
    SemaphoreHandle_t _s;
    MutexGuard _g;
};
}  // namespace

// ---------------------------------------------------------------------------
FlowRunLogger::FlowRunLogger() {
    _mutex = xSemaphoreCreateMutex();
}

FlowRunLogger::~FlowRunLogger() {
    if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
}

// ---------------------------------------------------------------------------
void FlowRunLogger::_ensureDir() {
    if (!_fs) return;
    if (!_fs->exists(_dir)) _fs->mkdir(_dir);
}

void FlowRunLogger::_path(char* buf, size_t len) const {
    snprintf(buf, len, "%s/runs.txt", _dir);
}

// ---------------------------------------------------------------------------
void FlowRunLogger::begin(fs::FS& fs, const char* logDir, uint32_t maxSizeKB) {
    _fs = &fs;
    if (logDir && *logDir) {
        strncpy(_dir, logDir, sizeof(_dir) - 1);
        _dir[sizeof(_dir) - 1] = '\0';
    }
    _maxSizeKB = maxSizeKB ? maxSizeKB : 256;
    _ensureDir();
    Serial.printf("[FlowRunLogger] dir=%s maxKB=%lu idle=%lus start=%.2f L/min\n",
                  _dir, (unsigned long)_maxSizeKB,
                  (unsigned long)_idleTimeoutSec, _startThreshold);
}

// ---------------------------------------------------------------------------
void FlowRunLogger::_enforceSizeRotation() {
    if (!_fs || _maxSizeKB == 0) return;
    char path[80]; _path(path, sizeof(path));
    File f = _fs->open(path, "r");
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz > (size_t)_maxSizeKB * 1024UL) {
        char bak[96];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        MutexGuard guard(fsMutex, pdMS_TO_TICKS(2000));
        if (!guard.isLocked()) {
            Serial.printf("[FlowRunLogger] rotation skipped for %s (mutex timeout)\n", path);
            return;
        }
        if (_fs->rename(path, bak)) {
            // success — LittleFS overwrote atomically
        } else if (_fs->exists(bak) && _fs->remove(bak) && _fs->rename(path, bak)) {
            // SD/FAT fallback — non-atomic but works
        } else {
            Serial.printf("[FlowRunLogger] rotation failed for %s\n", path);
            return;
        }
        Serial.printf("[FlowRunLogger] rotated %s -> %s\n", path, bak);
    }
}

// ---------------------------------------------------------------------------
void FlowRunLogger::feed(const SensorReading& r, uint32_t epoch) {
    if (!isfinite(r.value)) return;
    if (r.quality == QUALITY_ERROR) return;

    bool isFlow = (strcmp(r.metric, "flow_rate") == 0);
    bool isVol  = (strcmp(r.metric, "volume")    == 0);
    if (!isFlow && !isVol) return;

    Lock lk(_mutex);
    if (!lk.ok()) return;

    if (isVol) {
        // Cumulative volume from the sensor.  Captured at run start; closing
        // delta = latest - start.
        _volumeLatest = r.value;
        return;
    }

    // flow_rate path
    float flow = r.value;
    if (flow >= _startThreshold) {
        _lastNonZeroTs = epoch;
        if (_state == IDLE) {
            _state         = RUNNING;
            _runStart      = epoch;
            _maxFlow       = flow;
            _flowSum       = 0.0;
            _flowCount     = 0;
            _volumeStart   = isfinite(_volumeLatest) ? _volumeLatest : 0.0f;
            Serial.printf("[FlowRunLogger] run START ts=%lu flow=%.2f L/min\n",
                          (unsigned long)epoch, flow);
        }
    }

    if (_state == RUNNING) {
        _flowSum += (double)flow;
        _flowCount++;
        if (flow > _maxFlow) _maxFlow = flow;
    }
}

// ---------------------------------------------------------------------------
void FlowRunLogger::tick(uint32_t epoch) {
    Lock lk(_mutex);
    if (!lk.ok()) return;

    if (_state != RUNNING) return;
    if (_lastNonZeroTs == 0) return;

    if (epoch >= _lastNonZeroTs + _idleTimeoutSec) {
        _closeRun(_lastNonZeroTs);
    }
}

// ---------------------------------------------------------------------------
void FlowRunLogger::_closeRun(uint32_t endTs) {
    if (_state != RUNNING) return;

    uint32_t duration = (endTs > _runStart) ? (endTs - _runStart) : 0;
    float    volume   = NAN;
    if (isfinite(_volumeStart) && isfinite(_volumeLatest))
        volume = _volumeLatest - _volumeStart;
    if (isfinite(volume) && volume < 0.0f) volume = 0.0f;
    float    meanFlow = (_flowCount > 0) ? (float)(_flowSum / (double)_flowCount) : NAN;

    char line[160];
    char volBuf[16] = "", meanBuf[16] = "", maxBuf[16] = "";
    if (isfinite(volume))   snprintf(volBuf,  sizeof(volBuf),  "%.3f", volume);
    if (isfinite(meanFlow)) snprintf(meanBuf, sizeof(meanBuf), "%.3f", meanFlow);
    if (isfinite(_maxFlow)) snprintf(maxBuf,  sizeof(maxBuf),  "%.3f", _maxFlow);

    int n = snprintf(line, sizeof(line), "%lu|%lu|%lu|%s|%s|%s",
                     (unsigned long)_runStart, (unsigned long)endTs,
                     (unsigned long)duration, volBuf, meanBuf, maxBuf);
    if (n <= 0 || n >= (int)sizeof(line)) {
        Serial.println("[FlowRunLogger] line build overflow — dropping run");
        _state = IDLE;
        return;
    }

    _ensureDir();
    _enforceSizeRotation();

    char path[80]; _path(path, sizeof(path));
    bool isNew = !_fs || !_fs->exists(path);

    if (_fs) {
        MutexGuard fsLock(fsMutex, pdMS_TO_TICKS(2000));
        if (!fsLock.isLocked()) {
            Serial.println("[FlowRunLogger] fsMutex timeout — dropping run");
            _state = IDLE;
            return;
        }
        File f = _fs->open(path, FILE_APPEND);
        if (!f) {
            Serial.printf("[FlowRunLogger] open FAILED: %s\n", path);
        } else {
            if (isNew) {
                static const char HDR[] =
                    "# start_ts|end_ts|duration_s|volume_L|mean_Lmin|max_Lmin";
                f.write((const uint8_t*)HDR, sizeof(HDR) - 1);
                f.write((uint8_t)'\n');
            }
            size_t want    = (size_t)n;
            size_t written = f.write((const uint8_t*)line, want);
            size_t nl      = f.write((uint8_t)'\n');
            f.flush();
            f.close();
            if (written != want || nl == 0) {
                Serial.printf("[FlowRunLogger] short write %u/%u (disk full?)\n",
                              (unsigned)written, (unsigned)want);
            } else {
                Serial.printf("[FlowRunLogger] run END ts=%lu dur=%lus vol=%s L\n",
                              (unsigned long)endTs, (unsigned long)duration, volBuf);
            }
        }
    }

    _state         = IDLE;
    _runStart      = 0;
    _lastNonZeroTs = 0;
    _maxFlow       = 0.0f;
    _flowSum       = 0.0;
    _flowCount     = 0;
    _volumeStart   = NAN;
}

// ---------------------------------------------------------------------------
bool FlowRunLogger::isRunning() const {
    Lock lk(_mutex);
    if (!lk.ok()) return false;
    return _state == RUNNING;
}

uint32_t FlowRunLogger::runStartEpoch() const {
    Lock lk(_mutex);
    if (!lk.ok()) return 0;
    return _runStart;
}
