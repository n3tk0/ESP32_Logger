#include "LiveAggregator.h"
#include <math.h>
#include <string.h>

float LiveAggregator::_kappaCorrect(float rawPm, float humidity, float kappa) {
    if (!isfinite(rawPm) || rawPm < 0.0f)            return rawPm;
    if (!isfinite(humidity) || humidity <= 0.0f)     return rawPm;
    float rh = humidity > 99.0f ? 99.0f : humidity;
    float factor = 1.0f + kappa * (rh / (100.0f - rh));
    if (factor <= 0.0f || !isfinite(factor)) return rawPm;
    return rawPm / factor;
}

void LiveAggregator::feed(const SensorReading& r, int col) {
    if (!isfinite(r.value)) return;
    if (r.quality == QUALITY_ERROR) return;

    if (strcmp(r.metric, "humidity") == 0) _lastHumidity = r.value;

    if (col < 0 || col >= MAX_COLUMNS) return;
    float val = r.value;
    if (_humCorr &&
        strcmp(r.sensorType, "sds011") == 0 &&
        (strcmp(r.metric, "pm25") == 0 || strcmp(r.metric, "pm10") == 0))
    {
        val = _kappaCorrect(val, _lastHumidity, _kappa);
    }
    if (!_count[col] || val < _min[col]) _min[col] = val;
    if (!_count[col] || val > _max[col]) _max[col] = val;
    _last[col] = val;
    _sum[col] += (double)val;
    _count[col]++;
}

void LiveAggregator::reset() {
    // min, max and last are read only where the count says a reading arrived.
    memset(_sum, 0, sizeof(_sum));
    memset(_count, 0, sizeof(_count));
    _lastFlushEpoch = 0;   // the next window starts at the next take()
}

bool LiveAggregator::take(uint32_t nowEpoch, bool force, float* vals,
                          uint32_t* windowStart, const uint8_t* modes, int nModes) {
    // First call, or the clock went backwards (an NTP correction): start the
    // window here rather than wait for the old baseline to come round again.
    if (_lastFlushEpoch == 0 || nowEpoch < _lastFlushEpoch) {
        _lastFlushEpoch = nowEpoch;
        if (!force) return false;
    }
    if (!force && nowEpoch < _lastFlushEpoch + _intervalSec) return false;

    bool any = false;
    for (uint8_t i = 0; i < MAX_COLUMNS; i++) {
        if (!_count[i]) { vals[i] = NAN; continue; }
        any = true;
        switch ((modes && i < nModes) ? modes[i] : (uint8_t)DL_AGG_AVG) {
            case DL_AGG_MIN:  vals[i] = _min[i];  break;
            case DL_AGG_MAX:  vals[i] = _max[i];  break;
            case DL_AGG_LAST: vals[i] = _last[i]; break;
            case DL_AGG_SUM:  vals[i] = (float)_sum[i]; break;
            default:          vals[i] = (float)(_sum[i] / (double)_count[i]); break;
        }
    }
    if (windowStart) *windowStart = _lastFlushEpoch;
    reset();
    _lastFlushEpoch = nowEpoch;
    return any;
}
