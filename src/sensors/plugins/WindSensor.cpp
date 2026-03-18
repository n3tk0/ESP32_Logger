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
    _enabled         = cfg["enabled"]          | true;
    _pin             = cfg["pin"]              | 8;
    _pulsesPerRev    = cfg["pulses_per_rev"]   | 1.0f;
    _metersPerRev    = cfg["meters_per_rev"]   | 0.5f;
    _sampleWindowMs  = cfg["sample_window_ms"] | 3000;

    if (_pulsesPerRev <= 0) _pulsesPerRev = 1.0f;

    pinMode(_pin, INPUT_PULLUP);
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t)_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)_pin, _isr, this);

    Serial.printf("[Wind] pin=%d ppr=%.1f mpr=%.2f window=%ums\n",
                  _pin, _pulsesPerRev, _metersPerRev, _sampleWindowMs);
    return true;
}

bool WindSensor::read(SensorReading& out) {
    SensorReading buf[1];
    if (readAll(buf, 1) < 1) return false;
    out = buf[0];
    return true;
}

int WindSensor::readAll(SensorReading* out, int maxOut) {
    if (!_enabled || maxOut < 1) return 0;

    // Reset counter, wait one sample window, count again
    noInterrupts();
    _pulses = 0;
    interrupts();

    delay(_sampleWindowMs);

    noInterrupts();
    uint32_t count = _pulses;
    interrupts();

    // revolutions in window
    float revs = (float)count / _pulsesPerRev;
    // speed = (revs * metersPerRev) / (windowSec)
    float windowSec = _sampleWindowMs / 1000.0f;
    float speed = (revs * _metersPerRev) / windowSec;

    out[0] = SensorReading::make(0, _id, getType(), "wind_speed", speed, "m/s");
    _lastReadTs = 0;
    return 1;
}
