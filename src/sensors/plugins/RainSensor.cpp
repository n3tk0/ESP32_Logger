#include "RainSensor.h"

void IRAM_ATTR RainSensor::_isr(void* arg) {
    RainSensor* self = static_cast<RainSensor*>(arg);
    uint32_t now = (uint32_t)micros();
    uint32_t dt  = now - self->_lastTipUs;
    if (dt >= ISR_DEBOUNCE_US) {
        self->_lastIntervalUs = dt;
        self->_lastTipUs      = now;
        self->_tips++;
    }
}

bool RainSensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _pin        = cfg["pin"]              | 9;
    _mmPerTip   = cfg["mm_per_pulse"]     | 0.2794f;
    _intervalMs = cfg["read_interval_ms"] | 60000;

    JsonObjectConst cal = cfg["calibration"];
    _calRate.load(cal, "rain_rate");
    _calTotal.load(cal, "rain_total");

    pinMode(_pin, INPUT_PULLUP);
    static bool _isrServiceInstalled = false;
    if (!_isrServiceInstalled) { gpio_install_isr_service(0); _isrServiceInstalled = true; }
    gpio_set_intr_type((gpio_num_t)_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add((gpio_num_t)_pin, _isr, this);

    DBGF("[Rain] pin=%d mm/tip=%.4f  cal_rate(%.2f+%.2fx)\n",
                  _pin, _mmPerTip, _calRate.offset, _calRate.scale);
    return true;
}

bool RainSensor::read(SensorReading& out) {
    SensorReading buf[2];
    if (readAll(buf, 2) < 1) return false;
    out = buf[0];
    return true;
}

int RainSensor::readAll(SensorReading* out, int maxOut) {
    if (!_enabled || maxOut < 2) return 0;

    noInterrupts();
    uint32_t tips       = _tips;
    uint32_t intervalUs = _lastIntervalUs;
    interrupts();

    float total = _calTotal.apply((float)tips * _mmPerTip);

    // Instantaneous rate: if last tip was recent, extrapolate to mm/h
    float rate = 0.0f;
    if (intervalUs > 0 && intervalUs < 3600000000UL) {
        float rawRate = _mmPerTip * 3600000000.0f / (float)intervalUs;
        rate = _calRate.apply(rawRate);
    }

    out[0] = SensorReading::make(0, _id, getType(), "rain_rate",  rate,  "mm/h");
    out[1] = SensorReading::make(0, _id, getType(), "rain_total", total, "mm");
    return 2;
}

void RainSensor::resetTotal() {
    noInterrupts();
    _tips           = 0;
    _lastTipUs      = 0;
    _lastIntervalUs = 0;
    interrupts();
}
