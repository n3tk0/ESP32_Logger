#include "BME688Sensor.h"
#include "../../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../I2CBus.h"
#include "../SensorManager.h"        // R17: _claim/_release helpers
#include "../ReadingCache.h"         // ambient temperature reference
#include "../../utils/Psychrometrics.h"
#include "../../utils/IaqBaselineStore.h"

bool BME688Sensor::init(JsonObjectConst cfg) {
    _enabled      = cfg["enabled"]            | true;
    _intervalMs   = cfg["read_interval_ms"]   | 10000;
    _addr         = (uint8_t)(cfg["address"]  | 0x76);
    _heaterTemp   = cfg["heater_temp"]        | 320;
    _heaterDurMs  = cfg["heater_duration_ms"] | 150;

    // Ambient temperature reference for the humidity correction.
    const char* ambSensor = cfg["ambient_temp_sensor"] | "";
    strlcpy(_ambientSensor, ambSensor, sizeof(_ambientSensor));
    const char* ambMetric = cfg["ambient_temp_metric"] | "temperature";
    strlcpy(_ambientMetric, ambMetric, sizeof(_ambientMetric));
    _ambientMaxAgeMs = cfg["ambient_max_age_ms"] | 60000;
    _ambientWarned   = false;

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    // I2C bus selection. acquire() validates both pins against the board
    // profile, rejects a bus this chip does not have, and refuses a bus
    // already brought up on different pins.
    _bus  = (uint8_t)(cfg["bus"] | 0);
    _wire = I2CBus::acquire(_bus, sda, scl, "bme688");
    if (!_wire) return false;
    if (!_claimI2cAddress(_bus, _addr, this)) {
        Log.printf("[BME688] I2C address 0x%02X already claimed on bus %u — refusing init\n", _addr, (unsigned)_bus);
        return false;
    }

    JsonObjectConst cal = cfg["calibration"];
    _calTemp.load(cal, "temperature");
    _calHumidity.load(cal, "humidity");
    _calPressure.load(cal, "pressure");
    _calGas.load(cal, "gas_resistance");

    if (!_bme.begin(_addr, _wire)) {
        Log.printf("[BME688] Not found at 0x%02X\n", _addr);
        return false;
    }

    // Configure sensor oversampling and filter
    _bme.setTemperatureOversampling(BME688_Mini::OS_8X);
    _bme.setHumidityOversampling(BME688_Mini::OS_2X);
    _bme.setPressureOversampling(BME688_Mini::OS_4X);
    _bme.setIIRFilterSize(BME688_Mini::FILTER_3);
    _bme.setGasHeater(_heaterTemp, _heaterDurMs);

    // New settings start from a clean slate: a baseline taken under other
    // heater settings (or another sensor id) must not carry over in memory.
    // _loadBaseline() restores one only when the file matches these settings.
    _iaq        = GasIaq();
    _initMs     = millis();
    _warm       = false;
    _loadBaseline();

    _ready = true;
    Log.printf("[BME688] Ready at 0x%02X heater=%d°C/%dms ambient_ref=%s\n",
                  _addr, _heaterTemp, _heaterDurMs,
                  _ambientSensor[0] ? _ambientSensor : "(self)");
    return true;
}

bool BME688Sensor::read(SensorReading& out) {
    SensorReading buf[7];
    if (readAll(buf, 7) < 1) return false;
    out = buf[0];
    return true;
}

// ---------------------------------------------------------------------------
// Air temperature to express the humidity against. Falls back to `fallbackC`
// (this sensor's own calibrated temperature) whenever the configured reference
// is missing, stale or implausible — a frozen last-known value would be worse
// than an admittedly self-heated one, because it looks equally healthy.
float BME688Sensor::_ambientTempC(float fallbackC) const {
    if (_ambientSensor[0] == '\0') return fallbackC;

    float    refC  = 0.0f;
    uint32_t ageMs = 0;
    if (!readingCache.get(_ambientSensor, _ambientMetric, refC, ageMs)) {
        if (!_ambientWarned) {
            Log.printf("[BME688] ambient ref '%s/%s' not seen yet — using own temperature\n",
                          _ambientSensor, _ambientMetric);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (ageMs > _ambientMaxAgeMs) {
        if (!_ambientWarned) {
            Log.printf("[BME688] ambient ref '%s/%s' stale (%lums) — using own temperature\n",
                          _ambientSensor, _ambientMetric, (unsigned long)ageMs);
            _ambientWarned = true;
        }
        return fallbackC;
    }
    if (!Psychro::isValidTemp(refC)) return fallbackC;

    _ambientWarned = false;   // arm the next warning
    return refC;
}

int BME688Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;

    // performReading() triggers a new forced-mode reading and waits for it
    if (!_bme.performReading()) return 0;

    float rawGas = (float)_bme.gas_resistance;          // Ω, uncalibrated
    float tDie = _bme.temperature;                      // die temperature, uncalibrated
    float t   = _calTemp.apply(tDie);
    float h   = _calHumidity.apply(_bme.humidity);
    float p   = _calPressure.apply(_bme.pressure / 100.0f);
    float g   = _calGas.apply(rawGas);
    float iaq = _iaq.update(h, rawGas, _warmedUp());  // 0..500 (lower = cleaner)
    float tvoc = _iaq.tvocPpb(rawGas);                 // ppb, estimate
    _maybeSaveBaseline();

    // Dew point pairs the RH with the temperature it was measured AT — the raw
    // die temperature, not the calibrated one. Pairing it with a corrected
    // temperature would bake the self-heating error into the dew point, which
    // is the one figure that is supposed to be free of it.
    float dew  = Psychro::dewPointC(tDie, h);

    // Only compute — and only publish — the corrected humidity when something
    // is actually correcting. Unconfigured, _ambientTempC(t) returns tDie and
    // rhAtTempC(dewPointC(tDie, h), tDie) is h by construction, so the metric
    // would be a duplicate of "humidity" paid for in ring-buffer bytes.
    const bool corrected = _correctionConfigured();
    float hAmb = (corrected && isfinite(dew))
                     ? Psychro::rhAtTempC(dew, _ambientTempC(t)) : NAN;

    int n = 0;
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "temperature",    t,   "C");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "humidity",       h,   "%");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "pressure",       p,   "hPa");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "gas_resistance", g,   "Ohm");
    if (n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "iaq",            iaq, "");
    if (isfinite(tvoc) && n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "tvoc_est", tvoc, "ppb");
    // Dew point stays unconditional: it is an ambient property in its own
    // right, not a restatement of the humidity. Both are dropped rather than
    // emitted as NaN when the inputs are out of range — a NaN would land in
    // storage as a permanent null.
    if (isfinite(dew)  && n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "dew_point",    dew,  "C");
    if (isfinite(hAmb) && n < maxOut) out[n++] = SensorReading::make(0, _id, getType(), "humidity_amb", hAmb, "%");
    return n;
}

// ---------------------------------------------------------------------------
// Gas baseline persistence (utils/IaqBaselineStore.h).
namespace {
constexpr uint32_t IAQ_WARMUP_MS = 30UL * 60UL * 1000UL;  // heater settle time
}

// Latched, so the 49.7-day millis() wrap does not bring the warm-up back.
bool BME688Sensor::_warmedUp() {
    if (!_warm && millis() - _initMs >= IAQ_WARMUP_MS) _warm = true;
    return _warm;
}

void BME688Sensor::_loadBaseline() {
    char path[32];
    IaqBaselineStore::path("iaq", getId(), path, sizeof(path));
    _iaq.baseline = IaqBaselineStore::load(path, _heaterTemp, _heaterDurMs, "BME688");
    _saver.reset(millis(), _iaq.baseline);
}

// Runs from readAll() in SlowSensorTask (isBlocking), which holds no
// wireMutex, so waiting on fsMutex here stalls no other sensor.
void BME688Sensor::_maybeSaveBaseline() {
    // Readings taken while the heater is still settling are low and would
    // drag the stored baseline toward "polluted".
    if (!_warmedUp()) return;
    const uint32_t now = millis();
    if (!_saver.due(now, _iaq.baseline)) return;
    char path[32];
    IaqBaselineStore::path("iaq", getId(), path, sizeof(path));
    const bool ok = IaqBaselineStore::save(path, _heaterTemp, _heaterDurMs,
                                           _iaq.baseline, 2000);
    _saver.done(now, _iaq.baseline, ok);
    // fsMutex busy (a datalog flush) or a write error: the saver tries
    // again in five minutes instead of waiting out the hour.
    if (!ok) Log.println("[BME688] IAQ baseline save failed, retrying in 5 min");
}
