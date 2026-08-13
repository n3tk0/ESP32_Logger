#pragma once
#include "../ISensor.h"
#include <Wire.h>
#include "../../drivers/BME688_Mini.h"

// ============================================================================
// BME680 / BME688 — Temperature / Humidity / Pressure / Gas + IAQ (I2C)
// Uses the internal BME688_Mini driver (no Adafruit / BSEC dependency).
// One driver backs both the "bme688" and "bme680" plugin ids — same I2C
// protocol and MOX gas element; only the reading's type tag differs.
//
// Config keys:
//   "sda", "scl"           — I2C pins
//   "address"              — 0x76 (default) or 0x77
//   "read_interval_ms"     — polling interval (default 10000)
//   "heater_temp"          — gas heater target °C (default 320)
//   "heater_duration_ms"   — heater on duration (default 150)
//   "ambient_temp_sensor"  — id of a sensor giving the TRUE air temperature,
//                            typically a DS18B20 mounted outside any heated
//                            zone. Used only to re-express humidity; "" (the
//                            default) falls back to this sensor's own
//                            calibrated temperature.
//   "ambient_temp_metric"  — metric on that sensor (default "temperature";
//                            use "temperature_1" for the second probe on a
//                            shared 1-Wire bus)
//   "ambient_max_age_ms"   — reject the reference above this age (default
//                            60000). A dead probe must not silently freeze
//                            the correction at its last value.
//   "calibration": {
//       "temperature":    {"offset": 0.0, "scale": 1.0},
//       "humidity":       {"offset": 0.0, "scale": 1.0},
//       "pressure":       {"offset": 0.0, "scale": 1.0},
//       "gas_resistance": {"offset": 0.0, "scale": 1.0}
//   }
//
// SELF-HEATING AND HUMIDITY
// -------------------------
// "humidity" is the RH at the sensing element, which sits on a die warmed by
// the MOX gas heater, the board regulator, and any enclosure heating. Warm air
// at a fixed water content shows a lower RH, so this figure reads low relative
// to ambient — around 6 % relative per °C of self-heat near room temperature.
//
// A "calibration.temperature.offset" does NOT fix it: BME688_Mini computes
// humidity from _t_fine during performReading(), before any calibration axis
// is applied. That ordering is correct, because the element really is at the
// die temperature — but it means the offset only ever moves the reported
// temperature number.
//
// The dew point is unaffected by heating (it depends on absolute water content
// alone), so it is computed from the RAW die temperature paired with the RH
// measured at that same temperature, and then re-expressed as an RH at the
// ambient reference temperature. See utils/Psychrometrics.h.
//
// Produces up to 7 metrics:
//   "temperature"      °C
//   "humidity"         %    — RH at the sensing element (self-heated); this is
//                             the raw device figure, kept for continuity
//   "pressure"         hPa
//   "gas_resistance"   Ω    — MOX resistance (higher = cleaner air)
//   "iaq"              0..500 air-quality index (LOWER = cleaner; BSEC scale)
//                      derived from humidity + a self-calibrating gas baseline.
//                      NOTE: heuristic, not a Bosch-BSEC gas classification — it
//                      indicates overall air quality, not the specific gas type.
//   "dew_point"        °C   — true ambient dew point; invariant under heating
//   "humidity_ambient" %    — RH implied by that dew point at the ambient
//                             reference temperature. This is the figure to
//                             export to weather feeds.
//
// The last two are omitted from a sample when the inputs are out of range
// (e.g. a dead humidity element), rather than emitted as NaN.
// ============================================================================
class BME688Sensor : public ISensor {
public:
    // `type` lets the same driver back both the "bme688" and "bme680" plugin
    // ids (identical I2C protocol / MOX gas element); it only tags the readings.
    explicit BME688Sensor(const char* type = "bme688") : _type(type) {}

    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return _type; }
    const char* getName() const override { return "BME680/688 Environmental+Gas"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    bool        isBlocking() const override { return true; }  // performReading() blocks up to 1 s
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "temperature", "humidity", "pressure", "gas_resistance",
                                   "iaq", "dew_point", "humidity_ambient" };
        int n = 7; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    // IAQ (Indoor Air Quality) — derived 0..500 index (lower = cleaner air,
    // BSEC convention) from humidity + a self-calibrating gas-resistance
    // baseline. Heuristic only (no Bosch BSEC); accuracy ramps up over the
    // first minutes as the baseline settles.
    float _computeIaq(float humidity, float rawGasOhm);

    // Resolves the air temperature to express humidity against: the configured
    // reference sensor when it has a fresh reading, otherwise `fallbackC`.
    float _ambientTempC(float fallbackC) const;

    const char*  _type;                  // "bme688" or "bme680"
    BME688_Mini  _bme;
    char     _ambientSensor[17] = {};     // "" = use own calibrated temperature
    char     _ambientMetric[16] = "temperature";
    uint32_t _ambientMaxAgeMs   = 60000;
    // mutable: _ambientTempC() is const but latches this to keep the
    // stale-reference warning to one line per outage instead of one per read.
    mutable bool _ambientWarned = false;
    uint32_t _intervalMs   = 10000;
    uint8_t  _addr         = 0x76;
    int      _heaterTemp   = 320;
    int      _heaterDurMs  = 150;
    bool     _ready        = false;
    float    _gasBaseline  = 0.0f;        // clean-air resistance ceiling (Ω)

    CalibrationAxis _calTemp;
    CalibrationAxis _calHumidity;
    CalibrationAxis _calPressure;
    CalibrationAxis _calGas;
};
