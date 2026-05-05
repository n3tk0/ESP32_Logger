#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/SensorTypes.h"

// ============================================================================
// LiveAggregator — thread-safe RAM accumulator for wide-CSV logging.
//
// Per (sensorType, metric) pair, accumulates running sum + count.  Every
// `intervalSec` seconds (default 60), `buildRowIfDue()` emits a single
// wide-CSV row with one cell per registered column (empty if no samples
// arrived this window) and resets the accumulators.
//
// Optional SDS011 humidity correction (k-Köhler theory) is applied at
// feed-time using the most recent BME280 humidity reading:
//     factor = 1 + kappa * (RH / (100 - RH))
//     corrected_pm = raw_pm / factor
//
// Thread-safety: feed() / buildRowIfDue() / columns() all take an internal
// mutex.  Producer is StorageTask.  Web UI introspection is read-only and
// blocks for at most a few microseconds.
//
// Memory: fixed pool, no heap allocation after construction.  All CSV
// formatting uses snprintf into caller-supplied buffers (no Arduino String).
// ============================================================================
class LiveAggregator {
public:
    static constexpr uint8_t  MAX_COLUMNS    = 24;
    static constexpr uint8_t  COL_KEY_LEN    = 28;   // sensorType + '_' + metric
    static constexpr uint8_t  COL_HEADER_LEN = 28;
    static constexpr size_t   ROW_BUF_BYTES  = 320;  // recommended buffer size

    LiveAggregator();
    ~LiveAggregator();

    // No copy/move — owns a FreeRTOS semaphore.
    LiveAggregator(const LiveAggregator&)            = delete;
    LiveAggregator& operator=(const LiveAggregator&) = delete;

    // Configure flush cadence and SDS011 humidity correction.
    void setIntervalSec(uint16_t sec)            { _intervalSec = sec ? sec : 60; }
    void setHumidityCorrection(bool en, float k) { _humCorr = en; _kappa = (k > 0 ? k : 0.35f); }

    // Feed a single sensor reading.  Bad / NaN values are silently dropped.
    void feed(const SensorReading& r);

    // Format the current schema as a CSV header line (`timestamp,col1,col2,...`).
    // Returns the number of bytes written (excluding NUL), or -1 on overflow.
    int  buildHeader(char* buf, size_t bufLen);

    // If `nowEpoch - lastFlushEpoch >= intervalSec`, build the CSV row, reset
    // accumulators and return true.  On the very first call simply primes the
    // baseline epoch and returns false.  `outRowEpoch` receives the row's
    // timestamp (== nowEpoch) on success.
    bool buildRowIfDue(uint32_t nowEpoch, char* buf, size_t bufLen,
                       uint32_t* outRowEpoch);

    // Force-build a row regardless of cadence (used at shutdown).  Returns
    // false if no columns have any samples.
    bool flushNow(uint32_t nowEpoch, char* buf, size_t bufLen,
                  uint32_t* outRowEpoch);

    // Read-only column introspection for the web UI.  `keys` and `hdrs` are
    // caller-allocated arrays of fixed-width char rows.  Returns the number of
    // columns copied (capped by maxCols).
    size_t columns(char keys[][COL_KEY_LEN],
                   char hdrs[][COL_HEADER_LEN],
                   size_t maxCols) const;

    // Number of registered columns (for sizing UI).
    size_t columnCount() const;

    uint16_t intervalSec()        const { return _intervalSec; }
    bool     humidityCorrection() const { return _humCorr; }
    float    humidityKappa()      const { return _kappa; }

private:
    struct Col {
        char     header[COL_HEADER_LEN]; // CSV column name, e.g. "bme280_temperature"
        char     sensorType[12];
        char     metric[16];
        char     unit[12];
        char     ownerId[17];            // sensorId of the first contributor
        double   sum;
        uint32_t count;
        bool     used;                   // false until first feed()
    };

    Col      _cols[MAX_COLUMNS];
    uint8_t  _nCols       = 0;
    uint16_t _intervalSec = 60;
    bool     _humCorr     = false;
    float    _kappa       = 0.35f;
    uint32_t _lastFlushEpoch = 0;
    float    _lastHumidity   = NAN;     // most-recent BME-family humidity

    mutable SemaphoreHandle_t _mutex = nullptr;

    // Locates an existing column for (sensorType, metric, ownerId) or creates a
    // new one.  Returns -1 if the column pool is full.  Must be called with
    // `_mutex` already held.
    int  _findOrCreateCol(const char* id,
                          const char* sensorType,
                          const char* metric,
                          const char* unit);

    static void  _buildColumnHeader(char* dst, size_t dstLen,
                                    const char* sensorType,
                                    const char* metric);

    static float _kappaCorrect(float rawPm, float humidity, float kappa);

    // Build the CSV row text from current accumulators.  Caller holds _mutex.
    int  _writeRow(uint32_t epoch, char* buf, size_t bufLen);
    void _resetAccumulators();
};
