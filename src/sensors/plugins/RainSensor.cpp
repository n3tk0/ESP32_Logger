#include "RainSensor.h"
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin

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
    _pin        = cfg["pin"]              | -1;  // R11: unset → init refuses (closes AUDIT 23.1)
    _mmPerTip   = cfg["mm_per_pulse"]     | 0.2794f;
    _intervalMs = cfg["read_interval_ms"] | 60000;

    JsonObjectConst cal = cfg["calibration"];
    _calRate.load(cal, "rain_rate");
    _calTotal.load(cal, "rain_total");

    if (!validateAttachPin(_pin, "rain", "pin")) return false;
    pinMode(_pin, INPUT_PULLUP);
    if (!_isrPin.attach((uint8_t)_pin, GPIO_INTR_NEGEDGE, &RainSensor::_isr, this)) {
        Serial.printf("[Rain] ERROR: ISR attach failed on pin %d\n", _pin);
        return false;
    }

    Serial.printf("[Rain] pin=%d mm/tip=%.4f  cal_rate(%.2f+%.2fx)\n",
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
    uint32_t lastTipUs  = _lastTipUs;
    interrupts();

    float total = _calTotal.apply((float)tips * _mmPerTip);

    // Instantaneous rate: extrapolate the last inter-tip interval to mm/h, but
    // decay to zero once rain stops. If no tip has arrived for more than twice
    // the last interval, the bucket has clearly stopped tipping, so report 0
    // instead of holding a stale rate forever. The window scales with rain
    // intensity (mirrors WaterFlowSensor's self-clearing per-window delta) and
    // uses rollover-safe unsigned micros() subtraction.
    float rate = 0.0f;
    if (intervalUs > 0 && intervalUs < 3600000000UL) {
        uint32_t sinceLastTipUs = (uint32_t)micros() - lastTipUs;
        if ((uint64_t)sinceLastTipUs <= 2ULL * (uint64_t)intervalUs) {
            float rawRate = _mmPerTip * 3600000000.0f / (float)intervalUs;
            rate = _calRate.apply(rawRate);
        }
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
