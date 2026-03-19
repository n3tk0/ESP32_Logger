#include "WindSensor.h"

void IRAM_ATTR WindSensor::_isr(void* arg) {
    WindSensor* self = static_cast<WindSensor*>(arg);
    uint32_t now = (uint32_t)micros();
    if (now - self->_lastPulseUs >= ISR_DEBOUNCE_US) {
        self->_pulses++;
        self->_lastPulseUs = now;
    }
}

bool WindSensor::init(JsonObjectConst cfg) {
    _enabled        = cfg["enabled"]          | true;
    _pin            = cfg["pin"]              | 8;
    _pulsesPerRev   = cfg["pulses_per_rev"]   | 1.0f;
    _metersPerRev   = cfg["meters_per_rev"]   | 0.5f;
    _sampleWindowMs = cfg["sample_window_ms"] | 3000;
    _dirPin         = cfg["dir_pin"]          | -1;
    _dirMinVal      = cfg["dir_min_val"]      | 0;
    _dirMaxVal      = cfg["dir_max_val"]      | 4095;

    if (_pulsesPerRev <= 0) _pulsesPerRev = 1.0f;
    if (_dirMaxVal <= _dirMinVal) _dirMaxVal = _dirMinVal + 4095;

    JsonObjectConst cal = cfg["calibration"];
    _calSpeed.load(cal, "wind_speed");

    pinMode(_pin, INPUT_PULLUP);
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t)_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)_pin, _isr, this);

    if (_dirPin >= 0) {
        pinMode(_dirPin, INPUT);
        Serial.printf("[Wind] speed pin=%d  dir pin=%d  dir_range=[%d,%d]\n",
                      _pin, _dirPin, _dirMinVal, _dirMaxVal);
    } else {
        Serial.printf("[Wind] speed pin=%d ppr=%.1f mpr=%.2f window=%ums\n",
                      _pin, _pulsesPerRev, _metersPerRev, _sampleWindowMs);
    }
    return true;
}

bool WindSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int WindSensor::readAll(SensorReading* out, int maxOut) {
    if (!_enabled || maxOut < 1) return 0;

    // Reset counter, wait one sample window, count pulses
    noInterrupts();
    _pulses = 0;
    interrupts();

    delay(_sampleWindowMs);

    noInterrupts();
    uint32_t count = _pulses;
    interrupts();

    float revs      = (float)count / _pulsesPerRev;
    float windowSec = _sampleWindowMs / 1000.0f;
    float speed     = _calSpeed.apply((revs * _metersPerRev) / windowSec);

    out[0] = SensorReading::make(0, _id, getType(), "wind_speed", speed, "m/s");
    int n = 1;

    // Optionally read wind direction via AH49E hall sensor
    if (_dirPin >= 0 && maxOut >= 2) {
        int raw = analogRead(_dirPin);
        // Clamp to calibrated range
        raw = constrain(raw, _dirMinVal, _dirMaxVal);
        // Map ADC reading to 0–360°
        float angle = (float)(raw - _dirMinVal)
                      / (float)(_dirMaxVal - _dirMinVal) * 360.0f;
        out[1] = SensorReading::make(0, _id, getType(), "wind_direction", angle, "deg");
        n = 2;
    }

    _lastReadTs = 0;
    return n;
}
