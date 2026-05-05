#include "LiveAggregator.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// RAII guard for the internal semaphore.  Falls through silently when the
// semaphore could not be created (the caller still gets correct semantics on
// single-threaded boards but loses cross-task safety).
// ---------------------------------------------------------------------------
namespace {
class Lock {
public:
    Lock(SemaphoreHandle_t s, TickType_t timeout = portMAX_DELAY)
        : _s(s), _held(false)
    {
        if (_s) _held = (xSemaphoreTake(_s, timeout) == pdTRUE);
    }
    ~Lock() { if (_s && _held) xSemaphoreGive(_s); }
    bool ok() const { return _s == nullptr || _held; }
private:
    SemaphoreHandle_t _s;
    bool              _held;
};
}

// ---------------------------------------------------------------------------
LiveAggregator::LiveAggregator() {
    memset(_cols, 0, sizeof(_cols));
    _mutex = xSemaphoreCreateMutex();
}

LiveAggregator::~LiveAggregator() {
    if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
}

// ---------------------------------------------------------------------------
void LiveAggregator::_buildColumnHeader(char* dst, size_t dstLen,
                                         const char* sensorType,
                                         const char* metric)
{
    if (!dst || dstLen == 0) return;
    if (!sensorType) sensorType = "x";
    if (!metric)     metric     = "v";
    snprintf(dst, dstLen, "%s_%s", sensorType, metric);
    dst[dstLen - 1] = '\0';
}

// ---------------------------------------------------------------------------
float LiveAggregator::_kappaCorrect(float rawPm, float humidity, float kappa) {
    if (!isfinite(rawPm) || rawPm < 0.0f)            return rawPm;
    if (!isfinite(humidity) || humidity <= 0.0f)     return rawPm;
    // Avoid singularity near 100% RH and protect against bogus over-100% values.
    float rh = humidity > 99.0f ? 99.0f : humidity;
    float factor = 1.0f + kappa * (rh / (100.0f - rh));
    if (factor <= 0.0f || !isfinite(factor)) return rawPm;
    return rawPm / factor;
}

// ---------------------------------------------------------------------------
int LiveAggregator::_findOrCreateCol(const char* id,
                                      const char* sensorType,
                                      const char* metric,
                                      const char* unit)
{
    char header[COL_HEADER_LEN];
    _buildColumnHeader(header, sizeof(header), sensorType, metric);

    // First pass: exact header + same ownerId
    for (uint8_t i = 0; i < _nCols; i++) {
        if (strncmp(_cols[i].header, header, sizeof(_cols[i].header)) == 0 &&
            strncmp(_cols[i].ownerId, id, sizeof(_cols[i].ownerId)) == 0) {
            return (int)i;
        }
    }
    // Second pass: same header, different ownerId → disambiguate by appending
    // the id suffix to a freshly-allocated column.
    bool collision = false;
    for (uint8_t i = 0; i < _nCols; i++) {
        if (strncmp(_cols[i].header, header, sizeof(_cols[i].header)) == 0) {
            collision = true;
            break;
        }
    }

    if (_nCols >= MAX_COLUMNS) return -1;
    Col& c = _cols[_nCols];
    memset(&c, 0, sizeof(c));
    if (collision) {
        // header_<short_id>
        char tmp[COL_HEADER_LEN];
        snprintf(tmp, sizeof(tmp), "%s_%s", header, id ? id : "x");
        strncpy(c.header, tmp, sizeof(c.header) - 1);
    } else {
        strncpy(c.header, header, sizeof(c.header) - 1);
    }
    strncpy(c.sensorType, sensorType ? sensorType : "", sizeof(c.sensorType) - 1);
    strncpy(c.metric,     metric     ? metric     : "", sizeof(c.metric)     - 1);
    strncpy(c.unit,       unit       ? unit       : "", sizeof(c.unit)       - 1);
    strncpy(c.ownerId,    id         ? id         : "", sizeof(c.ownerId)    - 1);
    return (int)_nCols++;
}

// ---------------------------------------------------------------------------
void LiveAggregator::feed(const SensorReading& r) {
    if (!isfinite(r.value)) return;
    if (r.quality == QUALITY_ERROR) return;

    Lock lk(_mutex);
    if (!lk.ok()) return;

    // Track latest BME-family humidity for SDS011 correction.
    if (strcmp(r.metric, "humidity") == 0 && isfinite(r.value)) {
        _lastHumidity = r.value;
    }

    float val = r.value;
    if (_humCorr &&
        strcmp(r.sensorType, "sds011") == 0 &&
        (strcmp(r.metric, "pm25") == 0 || strcmp(r.metric, "pm10") == 0))
    {
        val = _kappaCorrect(val, _lastHumidity, _kappa);
    }

    int idx = _findOrCreateCol(r.sensorId, r.sensorType, r.metric, r.unit);
    if (idx < 0) return;  // pool full

    Col& c = _cols[idx];
    c.sum  += (double)val;
    c.count++;
    c.used  = true;
}

// ---------------------------------------------------------------------------
int LiveAggregator::buildHeader(char* buf, size_t bufLen) {
    if (!buf || bufLen < 16) return -1;
    Lock lk(_mutex);
    if (!lk.ok()) return -1;

    int n = snprintf(buf, bufLen, "timestamp");
    for (uint8_t i = 0; i < _nCols; i++) {
        if ((size_t)n >= bufLen) return -1;
        int w = snprintf(buf + n, bufLen - n, ",%s", _cols[i].header);
        if (w < 0 || (size_t)(n + w) >= bufLen) return -1;
        n += w;
    }
    return n;
}

// ---------------------------------------------------------------------------
int LiveAggregator::_writeRow(uint32_t epoch, char* buf, size_t bufLen) {
    int n = snprintf(buf, bufLen, "%lu", (unsigned long)epoch);
    if (n < 0 || (size_t)n >= bufLen) return -1;

    for (uint8_t i = 0; i < _nCols; i++) {
        const Col& c = _cols[i];
        int w;
        if (!c.used || c.count == 0) {
            w = snprintf(buf + n, bufLen - n, ",");
        } else {
            float avg = (float)(c.sum / (double)c.count);
            // %.4g keeps 4 significant digits, drops trailing zeros — compact
            // and lossless for typical sensor ranges.
            w = snprintf(buf + n, bufLen - n, ",%.4g", avg);
        }
        if (w < 0 || (size_t)(n + w) >= bufLen) return -1;
        n += w;
    }
    return n;
}

// ---------------------------------------------------------------------------
void LiveAggregator::_resetAccumulators() {
    for (uint8_t i = 0; i < _nCols; i++) {
        _cols[i].sum   = 0.0;
        _cols[i].count = 0;
        _cols[i].used  = false;
    }
}

// ---------------------------------------------------------------------------
bool LiveAggregator::buildRowIfDue(uint32_t nowEpoch, char* buf, size_t bufLen,
                                    uint32_t* outRowEpoch)
{
    Lock lk(_mutex);
    if (!lk.ok()) return false;

    if (_lastFlushEpoch == 0) {
        _lastFlushEpoch = nowEpoch;
        return false;
    }
    if (nowEpoch < _lastFlushEpoch + _intervalSec) return false;

    bool anySamples = false;
    for (uint8_t i = 0; i < _nCols; i++) if (_cols[i].used) { anySamples = true; break; }
    if (!anySamples) {
        _lastFlushEpoch = nowEpoch;  // advance baseline so we don't backlog
        return false;
    }

    int n = _writeRow(nowEpoch, buf, bufLen);
    _resetAccumulators();
    _lastFlushEpoch = nowEpoch;
    if (outRowEpoch) *outRowEpoch = nowEpoch;
    return n > 0;
}

// ---------------------------------------------------------------------------
bool LiveAggregator::flushNow(uint32_t nowEpoch, char* buf, size_t bufLen,
                               uint32_t* outRowEpoch)
{
    Lock lk(_mutex);
    if (!lk.ok()) return false;

    bool anySamples = false;
    for (uint8_t i = 0; i < _nCols; i++) if (_cols[i].used) { anySamples = true; break; }
    if (!anySamples) return false;

    int n = _writeRow(nowEpoch, buf, bufLen);
    _resetAccumulators();
    _lastFlushEpoch = nowEpoch;
    if (outRowEpoch) *outRowEpoch = nowEpoch;
    return n > 0;
}

// ---------------------------------------------------------------------------
size_t LiveAggregator::columns(char keys[][COL_KEY_LEN],
                                char hdrs[][COL_HEADER_LEN],
                                size_t maxCols) const
{
    Lock lk(_mutex);
    if (!lk.ok()) return 0;

    size_t n = (_nCols < maxCols) ? _nCols : maxCols;
    for (size_t i = 0; i < n; i++) {
        snprintf(keys[i], COL_KEY_LEN, "%s|%s",
                 _cols[i].sensorType, _cols[i].metric);
        keys[i][COL_KEY_LEN - 1] = '\0';
        strncpy(hdrs[i], _cols[i].header, COL_HEADER_LEN - 1);
        hdrs[i][COL_HEADER_LEN - 1] = '\0';
    }
    return n;
}

// ---------------------------------------------------------------------------
size_t LiveAggregator::columnCount() const {
    Lock lk(_mutex);
    if (!lk.ok()) return 0;
    return _nCols;
}
