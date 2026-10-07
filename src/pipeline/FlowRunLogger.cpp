#include "FlowRunLogger.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
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
            Log.printf("[FlowRunLogger] run START ts=%lu flow=%.2f L/min\n",
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
    // One row of the data log. The mean and peak flow the old runs.txt
    // carried are not data log fields; the flow rate itself is a sensor
    // column when it is logged.
    _done = DatalogRow{};
    _done.start  = _runStart;
    _done.end    = endTs;
    _done.volume = volume;
    _done.ff     = -1;
    _done.pf     = -1;
    strlcpy(_done.trigger, DL_TRIGGER_FLOW, sizeof(_done.trigger));
    _hasDone = true;
    Log.printf("[FlowRunLogger] run END ts=%lu dur=%lus vol=%.3f L\n",
                  (unsigned long)endTs, (unsigned long)duration, volume);

    _state         = IDLE;
    _runStart      = 0;
    _lastNonZeroTs = 0;
    _maxFlow       = 0.0f;
    _flowSum       = 0.0;
    _flowCount     = 0;
    _volumeStart   = NAN;
}

// ---------------------------------------------------------------------------
bool FlowRunLogger::takeRun(DatalogRow& out) {
    Lock lk(_mutex);
    if (!lk.ok() || !_hasDone) return false;
    out      = _done;
    _hasDone = false;
    return true;
}
