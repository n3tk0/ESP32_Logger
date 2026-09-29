#pragma once
#include <Arduino.h>
#include "../core/SensorTypes.h"
#include "../storage/Datalog.h"      // DL_MAX_COLS

// ============================================================================
// LiveAggregator — RAM averages for the data log's sensor columns.
//
// StorageTask resolves each reading to its data log column
// (datalogColsLearn) and feeds it here; every `intervalSec` seconds take()
// hands back one average per column (NAN where nothing arrived) and starts a
// new window. The row itself is formatted by DatalogFormat.
//
// Optional SDS011 humidity correction (k-Köhler theory) is applied at
// feed-time using the most recent humidity reading:
//     factor = 1 + kappa * (RH / (100 - RH))
//     corrected_pm = raw_pm / factor
//
// Owned by StorageTask alone, so it has no lock. Fixed arrays, no heap.
// ============================================================================
class LiveAggregator {
public:
    static constexpr uint8_t MAX_COLUMNS = DL_MAX_COLS;

    void setIntervalSec(uint16_t sec)            { _intervalSec = sec ? sec : 60; }
    void setHumidityCorrection(bool en, float k) { _humCorr = en; _kappa = (k > 0 ? k : 0.35f); }

    // Adds a reading to column `col`. Bad / NaN values are dropped; the
    // humidity is remembered for the SDS011 correction whatever `col` is.
    void feed(const SensorReading& r, int col);

    // When the window is due, or `force` and it holds any sample: fills
    // vals[MAX_COLUMNS] with the averages, *windowStart with when the window
    // began, starts the next window and returns true. The first call only
    // sets the window's start.
    bool take(uint32_t nowEpoch, bool force, float* vals, uint32_t* windowStart);

    // Throws the window away (the columns were renumbered).
    void reset();

    uint16_t intervalSec()        const { return _intervalSec; }
    bool     humidityCorrection() const { return _humCorr; }
    float    humidityKappa()      const { return _kappa; }

private:
    double   _sum[MAX_COLUMNS]   = {};
    uint32_t _count[MAX_COLUMNS] = {};
    uint16_t _intervalSec    = 60;
    bool     _humCorr        = false;
    float    _kappa          = 0.35f;
    uint32_t _lastFlushEpoch = 0;
    float    _lastHumidity   = NAN;

    static float _kappaCorrect(float rawPm, float humidity, float kappa);
};
