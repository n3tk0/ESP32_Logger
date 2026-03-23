#include "HCSR04Sensor.h"

bool HCSR04Sensor::init(JsonObjectConst cfg) {
    _enabled       = cfg["enabled"]           | true;
    _trigPin       = cfg["trig_pin"]          | 5;
    _echoPin       = cfg["echo_pin"]          | 4;
    _maxDistanceCm = cfg["max_distance_cm"]   | 400.0f;
    _intervalMs    = cfg["read_interval_ms"]  | 1000;

    JsonObjectConst cal = cfg["calibration"];
    _calDistance.load(cal, "distance");

    pinMode(_trigPin, OUTPUT);
    pinMode(_echoPin, INPUT);
    digitalWrite(_trigPin, LOW);
    delayMicroseconds(2);

    _ready = true;
    Serial.printf("[HC-SR04] trig=%d echo=%d max=%.0fcm\n",
                  _trigPin, _echoPin, _maxDistanceCm);
    return true;
}

float HCSR04Sensor::_measureCm() {
    // Ensure trigger is LOW before starting
    digitalWrite(_trigPin, LOW);
    delayMicroseconds(2);

    // 10µs HIGH trigger pulse
    digitalWrite(_trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(_trigPin, LOW);

    // Measure echo pulse width (HIGH duration)
    unsigned long duration = pulseIn(_echoPin, HIGH, ECHO_TIMEOUT_US);
    if (duration == 0) return -1.0f; // timeout / no object

    // distance = duration * speed_of_sound / 2
    // speed of sound = 0.034 cm/µs
    return (float)duration * 0.034f / 2.0f;
}

bool HCSR04Sensor::read(SensorReading& out) {
    SensorReading buf[1];
    if (readAll(buf, 1) < 1) return false;
    out = buf[0];
    return true;
}

int HCSR04Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;

    float cm = _measureCm();
    if (cm < 0 || cm > _maxDistanceCm) return 0;

    cm = _calDistance.apply(cm);
    out[0] = SensorReading::make(0, _id, getType(), "distance", cm, "cm");
    return 1;
}
