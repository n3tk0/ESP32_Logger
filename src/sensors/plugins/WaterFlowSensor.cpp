#include "WaterFlowSensor.h"

// ---------------------------------------------------------------------------
void IRAM_ATTR WaterFlowSensor::_isr(void* arg) {
    WaterFlowSensor* self = static_cast<WaterFlowSensor*>(arg);
    uint32_t now = (uint32_t)micros();
    if (now - self->_lastPulseUs >= ISR_DEBOUNCE_US) {
        self->_pulses++;
        self->_lastPulseUs = now;
    }
}

// ---------------------------------------------------------------------------
bool WaterFlowSensor::init(JsonObjectConst cfg) {
    _enabled        = cfg["enabled"]           | true;
    _pin            = cfg["pin"]               | 21;
    _pulsesPerLiter = cfg["pulses_per_liter"]  | _defaultPPL;
    _calibration    = cfg["calibration"]       | 1.0f;
    _intervalMs     = cfg["read_interval_ms"]  | 1000;

    // For custom type (defaultPPL == 0), pulses_per_liter is mandatory.
    if (_pulsesPerLiter <= 0.0f) {
        if (_defaultPPL == 0.0f) {
            Serial.printf("[%s] ERROR: 'pulses_per_liter' is required for custom "
                          "water flow sensor (e.g. 450.0 for YF-S201-equivalent)\n",
                          getType());
            return false;
        }
        _pulsesPerLiter = _defaultPPL;
    }
    if (_calibration <= 0.0f) _calibration = 1.0f;

    JsonObjectConst cal = cfg["cal"];
    _calFlow.load(cal, "flow_rate");
    _calVolume.load(cal, "volume");

    pinMode(_pin, INPUT_PULLUP);
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t)_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)_pin, _isr, this);

    _pulses        = 0;
    _lastPulseSnap = 0;
    _lastReadMs    = millis();

    Serial.printf("[%s] pin=%d ppl=%.1f cal=%.2f\n",
                  getType(), _pin, _pulsesPerLiter, _calibration);
    return true;
}

// ---------------------------------------------------------------------------
bool WaterFlowSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

// ---------------------------------------------------------------------------
int WaterFlowSensor::readAll(SensorReading* out, int maxOut) {
    if (!_enabled || maxOut < 2) return 0;

    uint32_t now  = millis();
    uint32_t dtMs = now - _lastReadMs;
    if (dtMs == 0) dtMs = 1;

    noInterrupts();
    uint32_t currentPulses = _pulses;
    interrupts();

    uint32_t deltaPulses = currentPulses - _lastPulseSnap;
    _lastPulseSnap = currentPulses;
    _lastReadMs    = now;

    // Instantaneous: (delta_pulses / ppl) * (60000 / dtMs) * legacyCal
    float rawRate  = ((float)deltaPulses / _pulsesPerLiter)
                     * (60000.0f / (float)dtMs)
                     * _calibration;
    float rawVol   = ((float)currentPulses / _pulsesPerLiter) * _calibration;

    float flowRate = _calFlow.apply(rawRate);
    float volume   = _calVolume.apply(rawVol);

    out[0] = SensorReading::make(0, _id, getType(), "flow_rate", flowRate, "L/min");
    out[1] = SensorReading::make(0, _id, getType(), "volume",    volume,   "L");
    _lastReadTs = 0;
    return 2;
}

// ---------------------------------------------------------------------------
void WaterFlowSensor::resetVolume() {
    noInterrupts();
    _pulses        = 0;
    _lastPulseSnap = 0;
    interrupts();
}

// ---------------------------------------------------------------------------
uint32_t WaterFlowSensor::rawPulseCount() const {
    noInterrupts();
    uint32_t c = _pulses;
    interrupts();
    return c;
}
