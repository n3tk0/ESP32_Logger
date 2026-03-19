#include "PMS5003Sensor.h"

bool PMS5003Sensor::init(JsonObjectConst cfg) {
    _enabled    = cfg["enabled"]           | true;
    _intervalMs = cfg["read_interval_ms"]  | 30000;
    int rx      = cfg["uart_rx"]           | 20;
    int tx      = cfg["uart_tx"]           | -1;
    int baud    = cfg["baud"]              | 9600;

    JsonObjectConst cal = cfg["calibration"];
    _calPm1.load(cal, "pm1");
    _calPm25.load(cal, "pm25");
    _calPm10.load(cal, "pm10");

    _serial = &Serial2;
    _serial->begin(baud, SERIAL_8N1, rx, tx);
    delay(100);
    while (_serial->available()) _serial->read();
    return true;
}

bool PMS5003Sensor::_readFrame() {
    if (!_serial) return false;
    uint8_t buf[FRAME_LEN];
    unsigned long deadline = millis() + 2000;

    int pos = 0;
    while (millis() < deadline) {
        if (!_serial->available()) { delay(5); continue; }
        uint8_t b = _serial->read();
        if (pos == 0 && b != START1) continue;
        if (pos == 1 && b != START2) { pos = 0; continue; }
        buf[pos++] = b;
        if (pos == FRAME_LEN) {
            // Validate checksum (sum of bytes 0..29 == bytes 30+31)
            uint16_t sum = 0;
            for (int i = 0; i < 30; i++) sum += buf[i];
            uint16_t chk = (buf[30] << 8) | buf[31];
            if (sum != chk) { pos = 0; continue; }

            // Atmospheric concentrations are at bytes 10,12,14 (uint16 BE)
            _pm1  = (float)((buf[10] << 8) | buf[11]);
            _pm25 = (float)((buf[12] << 8) | buf[13]);
            _pm10 = (float)((buf[14] << 8) | buf[15]);
            return true;
        }
    }
    return false;
}

bool PMS5003Sensor::read(SensorReading& out) {
    if (!_readFrame()) return false;
    out = SensorReading::make(0, _id, getType(), "pm25", _pm25, "ug/m3");
    return true;
}

int PMS5003Sensor::readAll(SensorReading* out, int maxOut) {
    if (maxOut < 3 || !_readFrame()) return 0;
    out[0] = SensorReading::make(0, _id, getType(), "pm1",  _calPm1.apply(_pm1),  "ug/m3");
    out[1] = SensorReading::make(0, _id, getType(), "pm25", _calPm25.apply(_pm25), "ug/m3");
    out[2] = SensorReading::make(0, _id, getType(), "pm10", _calPm10.apply(_pm10), "ug/m3");
    _lastReadTs = 0;
    return 3;
}
