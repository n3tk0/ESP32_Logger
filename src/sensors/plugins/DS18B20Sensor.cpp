#include "DS18B20Sensor.h"

// Metric names for up to 8 sensors: "temperature", "temperature_1" ... "temperature_7"
const char* DS18B20Sensor::_metricName(int idx) {
    static const char* names[] = {
        "temperature", "temperature_1", "temperature_2", "temperature_3",
        "temperature_4", "temperature_5", "temperature_6", "temperature_7"
    };
    if (idx < 0 || idx >= MAX_SENSORS) return "temperature";
    return names[idx];
}

int DS18B20Sensor::getMetrics(const char** out, int maxOut) const {
    int n = (_ds.deviceCount() > 0) ? _ds.deviceCount() : 1;
    if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; i++) out[i] = _metricName(i);
    return n;
}

bool DS18B20Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]          | true;
    _intervalMs = cfg["read_interval_ms"] | 5000;
    _resolution = (uint8_t)(cfg["resolution"] | 12);
    int pin     = cfg["pin"] | 2;

    if (_resolution < 9)  _resolution = 9;
    if (_resolution > 12) _resolution = 12;

    JsonObjectConst cal = cfg["calibration"];
    _calTemp.load(cal, "temperature");

    _ready = _ds.begin((uint8_t)pin, _resolution);
    int count = _ds.deviceCount();
    if (count == 0) {
        DBGF("[DS18B20] No sensors found on pin %d\n", pin);
        _ready = true;  // bus may have device connect later
    } else {
        DBGF("[DS18B20] Found %d sensor(s) on pin %d res=%d-bit\n",
                      count, pin, _resolution);
    }

    return true;
}

bool DS18B20Sensor::read(SensorReading& out) {
    SensorReading buf[MAX_SENSORS];
    if (readAll(buf, MAX_SENSORS) < 1) return false;
    out = buf[0];
    return true;
}

int DS18B20Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 1) return 0;

    int count = _ds.deviceCount();
    if (count == 0) return 0;

    // Request temperature conversion and wait
    _ds.requestTemperatures();
    delay(_ds.conversionTimeMs());

    int n = min(count, min(maxOut, MAX_SENSORS));
    int reported = 0;
    for (int i = 0; i < n; i++) {
        float t = _ds.getTempC(i);
        if (t == DS18B20_Mini::DISCONNECTED) continue;
        t = _calTemp.apply(t);
        out[reported++] = SensorReading::make(0, _id, getType(),
                                              _metricName(i), t, "C");
    }
    return reported;
}
