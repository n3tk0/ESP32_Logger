#include "HeaterModule.h"
#include "../core/BoardProfiles.h"
#include "../sensors/ReadingCache.h"
#include "../utils/Psychrometrics.h"
#include "../utils/MutexGuard.h"
#include <math.h>

namespace {

const char HEATER_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"pin\",\"type\":\"int\",\"min\":-1,\"max\":48,\"label\":\"MOSFET gate GPIO\",\"group\":\"Output\","
        "\"help\":\"-1 disables the output. Must not be a strapping pin: a boot-time pull-up there would energise the heater on every reset. Fit a 10k gate pull-down regardless.\"},"
      "{\"id\":\"invert\",\"type\":\"bool\",\"label\":\"Active-low drive\",\"group\":\"Output\","
        "\"help\":\"On for gate drivers or level shifters that invert. Leave off for a plain logic-level N-MOSFET.\"},"
      "{\"id\":\"pwmFreqHz\",\"type\":\"int\",\"min\":50,\"max\":2000,\"label\":\"PWM frequency\",\"unit\":\"Hz\",\"group\":\"Output\","
        "\"help\":\"200 Hz suits a resistive/PTC load. Higher only raises switching losses.\"},"
      "{\"id\":\"maxDutyPct\",\"type\":\"int\",\"min\":1,\"max\":100,\"label\":\"Max duty\",\"unit\":\"%\",\"group\":\"Output\","
        "\"help\":\"Caps the output. Use it to fit the heater inside the power budget of the supply.\"},"
      "{\"id\":\"softStartMs\",\"type\":\"int\",\"min\":0,\"max\":30000,\"label\":\"Soft start\",\"unit\":\"ms\",\"group\":\"Output\","
        "\"help\":\"Ramp time from 0 to target duty. A cold PTC draws several times its steady-state current; without a ramp that inrush trips small mains modules.\"},"
      "{\"id\":\"tempSensor\",\"type\":\"string\",\"max\":16,\"label\":\"Enclosure probe id\",\"group\":\"Control inputs\","
        "\"help\":\"Sensor id measuring the heated enclosure, e.g. the DS18B20 on the heater. Required.\"},"
      "{\"id\":\"tempMetric\",\"type\":\"string\",\"max\":15,\"label\":\"Enclosure metric\",\"group\":\"Control inputs\","
        "\"help\":\"Usually temperature. For the second probe on a shared 1-Wire bus use temperature_1.\"},"
      "{\"id\":\"dewSensor\",\"type\":\"string\",\"max\":16,\"label\":\"Dew-point source id\",\"group\":\"Control inputs\","
        "\"help\":\"Optional. Sensor id publishing dew_point, e.g. the BME680. Empty disables condensation protection.\"},"
      "{\"id\":\"dewMetric\",\"type\":\"string\",\"max\":15,\"label\":\"Dew-point metric\",\"group\":\"Control inputs\"},"
      "{\"id\":\"setpointC\",\"type\":\"float\",\"min\":-20,\"max\":60,\"step\":0.5,\"label\":\"Frost setpoint\",\"unit\":\"\\u00b0C\",\"group\":\"Control\","
        "\"help\":\"Minimum enclosure temperature. 5 \\u00b0C keeps an SPS30 well inside its -10 \\u00b0C floor without drying the sample air enough to bias PM readings.\"},"
      "{\"id\":\"dewMarginC\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.5,\"label\":\"Dew-point margin\",\"unit\":\"K\",\"group\":\"Control\","
        "\"help\":\"Hold the enclosure at least this far above the dew point.\"},"
      "{\"id\":\"hysteresisC\",\"type\":\"float\",\"min\":0.2,\"max\":10,\"step\":0.1,\"label\":\"Hysteresis\",\"unit\":\"K\",\"group\":\"Control\","
        "\"help\":\"Total width of the switching band around the target.\"},"
      "{\"id\":\"maxTempC\",\"type\":\"float\",\"min\":0,\"max\":80,\"step\":1,\"label\":\"Over-temperature cutoff\",\"unit\":\"\\u00b0C\",\"group\":\"Safety\","
        "\"help\":\"Latches the output off above this. Keep it below the sensor's maximum rating and below the thermal fuse.\"},"
      "{\"id\":\"staleTimeoutS\",\"type\":\"int\",\"min\":10,\"max\":3600,\"label\":\"Probe timeout\",\"unit\":\"s\",\"group\":\"Safety\","
        "\"help\":\"Force the heater off when the enclosure probe has been silent this long.\"}"
    "]}";

}  // namespace

// ---------------------------------------------------------------------------
const char* HeaterModule::_faultText(Fault f) {
    switch (f) {
        case FAULT_NO_PIN:     return "no output pin";
        case FAULT_STALE_TEMP: return "probe stale";
        case FAULT_OVERTEMP:   return "over-temperature";
        default:               return "";
    }
}

// ---------------------------------------------------------------------------
bool HeaterModule::load(JsonObjectConst cfg) {
    // Any config change forces the output off and a re-attach in tick(). A
    // half-applied config must never leave the previous pin driven.
    _forceOff(FAULT_NONE);

    if (!cfg["enabled"].isNull()) _enabled = cfg["enabled"] | false;

    int newPin = cfg["pin"] | _pin;
    if (newPin < -1 || newPin > 48) {
        Serial.printf("[Heater] pin %d out of range — rejected\n", newPin);
        return false;
    }
    if (newPin != _pin) _reconfigure = true;
    _pin = newPin;

    const char* s;
    s = cfg["tempSensor"] | (const char*)nullptr; if (s) strlcpy(_tempSensor, s, sizeof(_tempSensor));
    s = cfg["tempMetric"] | (const char*)nullptr; if (s) strlcpy(_tempMetric, s, sizeof(_tempMetric));
    s = cfg["dewSensor"]  | (const char*)nullptr; if (s) strlcpy(_dewSensor,  s, sizeof(_dewSensor));
    s = cfg["dewMetric"]  | (const char*)nullptr; if (s) strlcpy(_dewMetric,  s, sizeof(_dewMetric));
    if (_tempMetric[0] == '\0') strlcpy(_tempMetric, "temperature", sizeof(_tempMetric));
    if (_dewMetric[0]  == '\0') strlcpy(_dewMetric,  "dew_point",   sizeof(_dewMetric));

    _setpointC   = cfg["setpointC"]   | _setpointC;
    _hysteresisC = cfg["hysteresisC"] | _hysteresisC;
    _dewMarginC  = cfg["dewMarginC"]  | _dewMarginC;
    _maxTempC    = cfg["maxTempC"]    | _maxTempC;

    // Clamp rather than reject: these arrive from a UI form and a single
    // out-of-range field should not discard the whole payload. The values are
    // bounded to what the control law can act on safely.
    if (!isfinite(_setpointC)   || _setpointC < -20.0f || _setpointC > 60.0f) _setpointC   = 5.0f;
    if (!isfinite(_hysteresisC) || _hysteresisC < 0.2f || _hysteresisC > 10.0f) _hysteresisC = 1.5f;
    if (!isfinite(_dewMarginC)  || _dewMarginC < 0.0f  || _dewMarginC > 20.0f)  _dewMarginC  = 2.0f;
    if (!isfinite(_maxTempC)    || _maxTempC < 0.0f    || _maxTempC > 80.0f)    _maxTempC    = 45.0f;

    // An over-temperature cutoff at or below the setpoint would make the
    // heater unable to ever run; treat it as a misconfiguration and restore
    // a usable gap rather than silently never heating.
    if (_maxTempC <= _setpointC + OVERTEMP_CLEAR_MARGIN_C) {
        Serial.printf("[Heater] maxTempC %.1f too close to setpoint %.1f — raised\n",
                      _maxTempC, _setpointC);
        _maxTempC = _setpointC + OVERTEMP_CLEAR_MARGIN_C + 1.0f;
    }

    _staleTimeoutS = cfg["staleTimeoutS"] | _staleTimeoutS;
    if (_staleTimeoutS < 10 || _staleTimeoutS > 3600) _staleTimeoutS = 120;

    _softStartMs = cfg["softStartMs"] | _softStartMs;
    if (_softStartMs > 30000) _softStartMs = 3000;

    uint32_t newFreq = cfg["pwmFreqHz"] | _pwmFreqHz;
    if (newFreq < 50 || newFreq > 2000) newFreq = 200;
    if (newFreq != _pwmFreqHz) _reconfigure = true;
    _pwmFreqHz = newFreq;

    int maxDuty = cfg["maxDutyPct"] | (int)_maxDutyPct;
    if (maxDuty < 1)   maxDuty = 1;
    if (maxDuty > 100) maxDuty = 100;
    _maxDutyPct = (uint8_t)maxDuty;

    bool newInvert = cfg["invert"] | _invert;
    if (newInvert != _invert) _reconfigure = true;
    _invert = newInvert;

    return true;
}

// ---------------------------------------------------------------------------
bool HeaterModule::save(JsonObject cfg) const {
    cfg["enabled"]       = _enabled;
    cfg["pin"]           = _pin;
    cfg["tempSensor"]    = _tempSensor;
    cfg["tempMetric"]    = _tempMetric;
    cfg["dewSensor"]     = _dewSensor;
    cfg["dewMetric"]     = _dewMetric;
    cfg["setpointC"]     = _setpointC;
    cfg["hysteresisC"]   = _hysteresisC;
    cfg["dewMarginC"]    = _dewMarginC;
    cfg["maxTempC"]      = _maxTempC;
    cfg["staleTimeoutS"] = _staleTimeoutS;
    cfg["softStartMs"]   = _softStartMs;
    cfg["pwmFreqHz"]     = _pwmFreqHz;
    cfg["maxDutyPct"]    = _maxDutyPct;
    cfg["invert"]        = _invert;
    return true;
}

// ---------------------------------------------------------------------------
bool HeaterModule::start() {
    // Hardware setup is deferred to tick(): start() runs on the AsyncTCP
    // worker for a /api/modules/heater restart, and reconfiguring LEDC there
    // could race the control loop mid-write.
    _reconfigure = true;
    return true;
}

// ---------------------------------------------------------------------------
// Runs on the AsyncTCP worker (POST /api/modules/heater/{enable,restart}) while
// tick() may be mid-cycle on ProcessingTask.
//
// The output is killed FIRST and without any lock: stop() must guarantee the
// heater is de-energised by the time it returns, and making that guarantee
// contingent on acquiring a mutex would be exactly backwards for a safety
// path. Only the LEDC bookkeeping — which can wait — takes the lock, and if it
// cannot be had promptly the cleanup is handed to tick() instead of blocking
// a web request.
void HeaterModule::stop() {
    _dutyPct = 0;
    _heating = false;
    _safeOffNow();

    MutexGuard g(_hwMutex, pdMS_TO_TICKS(100));
    if (g.isLocked()) {
        _detachPin();
    } else {
        _reconfigure = true;   // tick() will detach under the lock
    }
}

// ---------------------------------------------------------------------------
// Drives the gate to its inactive level via plain GPIO. Works regardless of
// whether LEDC currently owns the pin: on ESP32 a digitalWrite to a
// LEDC-attached pin detaches the LEDC output for that GPIO, so this is a hard
// off rather than a request. Deliberately touches no shared bookkeeping.
void HeaterModule::_safeOffNow() {
    const int pin = (_attachedPin >= 0) ? _attachedPin : _pin;
    if (pin < 0) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, _invert ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// In-loop off path (tick() context, lock already held where it matters).
// Prefers the LEDC write so the pin stays attached and ready for the next
// on-edge; falls back to the hard GPIO off when LEDC does not own the pin.
void HeaterModule::_forceOff(Fault reason) {
    _dutyPct = 0;
    _heating = false;
    if (reason != FAULT_NONE) _fault = reason;
    if (_attached && _attachedPin >= 0) {
        _applyDuty(0);
    } else {
        _safeOffNow();
    }
}

// ---------------------------------------------------------------------------
bool HeaterModule::_attachPin() {
    _detachPin();
    if (_pin < 0) return false;

    if (!validateAttachPin(_pin, "heater", "pin")) return false;

    // Park the pin at the inactive level before LEDC takes it over, so the
    // gate is never left floating between pinMode and the first duty write.
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, _invert ? HIGH : LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach((uint8_t)_pin, _pwmFreqHz, PWM_RESOLUTION_BITS)) {
        Serial.printf("[Heater] ledcAttach failed on GPIO%d\n", _pin);
        return false;
    }
#else
    // Arduino core 2.x: explicit channel allocation. Channel 0 is the LEDC
    // channel the rest of this firmware leaves free.
    constexpr uint8_t LEDC_CHANNEL = 0;
    ledcSetup(LEDC_CHANNEL, _pwmFreqHz, PWM_RESOLUTION_BITS);
    ledcAttachPin((uint8_t)_pin, LEDC_CHANNEL);
#endif

    _attached    = true;
    _attachedPin = _pin;
    _applyDuty(0);
    Serial.printf("[Heater] attached GPIO%d @ %luHz %u-bit%s\n",
                  _pin, (unsigned long)_pwmFreqHz, PWM_RESOLUTION_BITS,
                  _invert ? " (active-low)" : "");
    return true;
}

// ---------------------------------------------------------------------------
void HeaterModule::_detachPin() {
    if (_attachedPin < 0) return;
    if (_attached) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcDetach((uint8_t)_attachedPin);
#else
        ledcDetachPin((uint8_t)_attachedPin);
#endif
    }
    // Leave the pin driven to the inactive level, not floating: a floating
    // gate on a power MOSFET can drift into partial conduction.
    pinMode(_attachedPin, OUTPUT);
    digitalWrite(_attachedPin, _invert ? HIGH : LOW);
    _attached    = false;
    _attachedPin = -1;
}

// ---------------------------------------------------------------------------
void HeaterModule::_applyDuty(uint8_t pct) {
    if (!_attached || _attachedPin < 0) return;
    if (pct > 100) pct = 100;
    uint32_t raw = (PWM_MAX_DUTY * pct) / 100;
    if (_invert) raw = PWM_MAX_DUTY - raw;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite((uint8_t)_attachedPin, raw);
#else
    ledcWrite(0, raw);
#endif
}

// ---------------------------------------------------------------------------
bool HeaterModule::_readInput(const char* sensorId, const char* metric,
                              uint32_t maxAgeMs, float& out) const {
    if (!sensorId || sensorId[0] == '\0') return false;
    float    v   = 0.0f;
    uint32_t age = 0;
    if (!readingCache.get(sensorId, metric, v, age)) return false;
    if (age > maxAgeMs) return false;
    if (!isfinite(v)) return false;
    out = v;
    return true;
}

// ---------------------------------------------------------------------------
void HeaterModule::tick(uint32_t nowMs) {
    if (_reconfigure) {
        // Take the lock before clearing the flag: on a failed acquire the flag
        // must survive so the next tick retries, rather than leaving the pin
        // in whatever half-configured state a concurrent stop() produced.
        MutexGuard g(_hwMutex, pdMS_TO_TICKS(50));
        if (g.isLocked()) {
            _reconfigure = false;
            _forceOff(FAULT_NONE);
            _detachPin();
            if (_enabled && _pin >= 0) {
                if (!_attachPin()) _fault = FAULT_NO_PIN;
            }
        } else {
            _safeOffNow();   // stay de-energised until the retry succeeds
            return;
        }
    }

    if (!_enabled) { _forceOff(FAULT_NONE); return; }

    if ((nowMs - _lastControlMs) < CONTROL_PERIOD_MS) return;
    _lastControlMs = nowMs;

    if (!_attached) { _forceOff(FAULT_NO_PIN); return; }

    // ---- Control input: enclosure temperature (mandatory) ------------------
    const uint32_t maxAgeMs = _staleTimeoutS * 1000UL;
    float tEncl = NAN;
    if (!_readInput(_tempSensor, _tempMetric, maxAgeMs, tEncl)) {
        if (_fault != FAULT_STALE_TEMP) {
            Serial.printf("[Heater] enclosure probe '%s/%s' unavailable — output forced off\n",
                          _tempSensor[0] ? _tempSensor : "(unset)", _tempMetric);
        }
        _lastTempC = NAN;
        _dewValid  = false;
        _forceOff(FAULT_STALE_TEMP);
        return;
    }
    _lastTempC = tEncl;

    // ---- Over-temperature latch -------------------------------------------
    if (tEncl >= _maxTempC) {
        if (!_overtempLatch) {
            Serial.printf("[Heater] OVER-TEMPERATURE %.1f\u00b0C >= %.1f\u00b0C — latched off\n",
                          tEncl, _maxTempC);
        }
        _overtempLatch = true;
    } else if (_overtempLatch && tEncl < (_maxTempC - OVERTEMP_CLEAR_MARGIN_C)) {
        Serial.printf("[Heater] over-temperature cleared at %.1f\u00b0C\n", tEncl);
        _overtempLatch = false;
    }
    if (_overtempLatch) { _forceOff(FAULT_OVERTEMP); return; }

    // ---- Control input: dew point (optional) -------------------------------
    float dew = NAN;
    _dewValid = _readInput(_dewSensor, _dewMetric, maxAgeMs, dew) &&
                Psychro::isValidTemp(dew);
    _lastDewC = _dewValid ? dew : NAN;

    // ---- Effective target --------------------------------------------------
    float target = _setpointC;
    if (_dewValid) {
        const float dewTarget = dew + _dewMarginC;
        if (dewTarget > target) target = dewTarget;
    }

    // ---- Hysteresis band ---------------------------------------------------
    const float half = _hysteresisC * 0.5f;
    bool heating = _heating;
    if (tEncl <= (target - half))      heating = true;
    else if (tEncl >= (target + half)) heating = false;
    // inside the band: hold the current state

    // ---- Edge handling + soft start ---------------------------------------
    if (heating && !_heating) {
        _rampStartMs = nowMs;
        _onSinceMs   = nowMs;
        Serial.printf("[Heater] ON  T=%.2f\u00b0C target=%.2f\u00b0C%s\n",
                      tEncl, target,
                      _dewValid ? " (dew-limited)" : "");
    } else if (!heating && _heating) {
        Serial.printf("[Heater] OFF T=%.2f\u00b0C target=%.2f\u00b0C\n", tEncl, target);
    }
    _heating = heating;
    _fault   = FAULT_NONE;

    if (!heating) { _dutyPct = 0; _applyDuty(0); return; }

    uint8_t duty = _maxDutyPct;
    if (_softStartMs > 0) {
        const uint32_t elapsed = nowMs - _rampStartMs;
        if (elapsed < _softStartMs) {
            duty = (uint8_t)((uint32_t)_maxDutyPct * elapsed / _softStartMs);
            if (duty < 1) duty = 1;   // never sit at exactly 0 while "on"
        }
    }
    _dutyPct = duty;
    _applyDuty(duty);
}

// ---------------------------------------------------------------------------
void HeaterModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;               // UI falls back to "disabled"

    if (_fault != FAULT_NONE) {
        out["text"] = _faultText(_fault);
        out["tone"] = (_fault == FAULT_OVERTEMP) ? "err" : "warn";
        return;
    }

    char buf[48];
    if (_heating) {
        snprintf(buf, sizeof(buf), "heating %u%% \u00b7 %.1f\u00b0C", _dutyPct, _lastTempC);
        out["text"] = buf;
        out["tone"] = "ok";
        return;
    }
    if (isfinite(_lastTempC)) {
        snprintf(buf, sizeof(buf), "idle \u00b7 %.1f\u00b0C", _lastTempC);
        out["text"] = buf;
        out["tone"] = "dim";
        return;
    }
    out["text"] = "waiting for probe";
    out["tone"] = "dim";
}

// ---------------------------------------------------------------------------
const char* HeaterModule::schema() const {
    return HEATER_SCHEMA;
}
