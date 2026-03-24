#include "YFS201Sensor.h"

// ---------------------------------------------------------------------------
void IRAM_ATTR YFS201Sensor::_isr(void* arg) {
    YFS201Sensor* self = static_cast<YFS201Sensor*>(arg);
    uint32_t now = (uint32_t)micros();
    if (now - self->_lastPulseUs >= ISR_DEBOUNCE_US) {
        self->_pulses++;
        self->_lastPulseUs = now;
    }
}

// ---------------------------------------------------------------------------
bool YFS201Sensor::init(JsonObjectConst cfg) {
    _enabled         = cfg["enabled"]           | true;
    _pin             = cfg["pin"]               | 21;
    _pulsesPerLiter  = cfg["pulses_per_liter"]  | 450.0f;
    _calibration     = cfg["calibration"]       | 1.0f;
    _intervalMs      = cfg["read_interval_ms"]  | 1000;

    if (_pulsesPerLiter <= 0.0f) _pulsesPerLiter = 450.0f;
    if (_calibration    <= 0.0f) _calibration    = 1.0f;

    pinMode(_pin, INPUT_PULLUP);
    // Attach ISR with this instance as argument (ESP-IDF style)
    static bool _isrServiceInstalled = false;
    if (!_isrServiceInstalled) { gpio_install_isr_service(0); _isrServiceInstalled = true; }
    gpio_set_intr_type((gpio_num_t)_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)_pin, _isr, this);

    _pulses        = 0;
    _lastPulseSnap = 0;
    _lastReadMs    = millis();

    DBGF("[YFS201] pin=%d ppl=%.1f cal=%.2f\n",
                  _pin, _pulsesPerLiter, _calibration);
    return true;
}

// ---------------------------------------------------------------------------
bool YFS201Sensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0]; // flow_rate
    return true;
}

// ---------------------------------------------------------------------------
int YFS201Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_enabled || maxOut < 2 || _pulsesPerLiter <= 0) return 0;

    uint32_t now    = millis();
    uint32_t dtMs   = now - _lastReadMs;
    if (dtMs == 0) dtMs = 1;

    noInterrupts();
    uint32_t currentPulses = _pulses;
    interrupts();

    uint32_t deltaPulses = currentPulses - _lastPulseSnap;
    _lastPulseSnap = currentPulses;
    _lastReadMs    = now;

    // Instantaneous flow rate: (pulses_in_interval / ppl) * (60000 / dtMs)
    float flowRate = ((float)deltaPulses / _pulsesPerLiter)
                     * (60000.0f / (float)dtMs)
                     * _calibration;

    // Cumulative volume
    float volume   = ((float)currentPulses / _pulsesPerLiter) * _calibration;

    out[0] = SensorReading::make(0, _id, getType(), "flow_rate", flowRate, "L/min");
    out[1] = SensorReading::make(0, _id, getType(), "volume",    volume,   "L");
    return 2;
}

// ---------------------------------------------------------------------------
void YFS201Sensor::resetVolume() {
    noInterrupts();
    _pulses        = 0;
    _lastPulseSnap = 0;
    interrupts();
}

// ---------------------------------------------------------------------------
uint32_t YFS201Sensor::rawPulseCount() const {
    noInterrupts();
    uint32_t c = _pulses;
    interrupts();
    return c;
}
