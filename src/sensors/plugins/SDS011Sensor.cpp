#include "SDS011Sensor.h"

// ---------------------------------------------------------------------------
bool SDS011Sensor::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | true;

    int rxPin = cfg["uart_rx"] | 20;
    int txPin = cfg["uart_tx"] | -1;
    int baud  = cfg["baud"]    | 9600;
    int work  = cfg["work_period_min"] | 1;
    _workPeriodMs = (uint32_t)work * 60000UL;
    if (_workPeriodMs == 0) _workPeriodMs = 1000;

    JsonObjectConst cal = cfg["calibration"];
    _calPm25.load(cal, "pm25");
    _calPm10.load(cal, "pm10");

    _serial = &Serial2;
    if (txPin >= 0) {
        _serial->begin(baud, SERIAL_8N1, rxPin, txPin);
    } else {
        _serial->begin(baud, SERIAL_8N1, rxPin, -1);
    }

    _drainBuffer();
    Serial.printf("[SDS011] RX=%d TX=%d baud=%d period=%lus\n",
                  rxPin, txPin, baud, _workPeriodMs / 1000);
    return true; // Can't verify HW without a valid frame yet
}

// ---------------------------------------------------------------------------
bool SDS011Sensor::_parseFrame(const uint8_t* buf) {
    // Validate header (0xAA), command (0xC0), tail (0xAB)
    if (buf[0] != 0xAA || buf[1] != 0xC0 || buf[9] != 0xAB) return false;

    // Checksum: sum of bytes 2..7
    uint8_t sum = 0;
    for (int i = 2; i < 8; i++) sum += buf[i];
    if (sum != buf[8]) return false;

    _pm25 = ((buf[3] << 8) | buf[2]) / 10.0f;
    _pm10 = ((buf[5] << 8) | buf[4]) / 10.0f;
    return true;
}

// ---------------------------------------------------------------------------
void SDS011Sensor::_drainBuffer() {
    if (!_serial) return;
    unsigned long t = millis();
    while (millis() - t < 100) {
        while (_serial->available()) _serial->read();
        delay(10);
    }
}

// ---------------------------------------------------------------------------
bool SDS011Sensor::read(SensorReading& out) {
    if (!_serial || !_enabled) return false;

    uint8_t buf[FRAME_LEN];
    int     pos = 0;
    unsigned long deadline = millis() + 1500;

    // Scan for 0xAA header then read full frame
    while (millis() < deadline) {
        if (!_serial->available()) { delay(5); continue; }
        uint8_t b = _serial->read();
        if (pos == 0 && b != 0xAA) continue;
        buf[pos++] = b;
        if (pos == FRAME_LEN) {
            if (_parseFrame(buf)) {
                _newData = true;
                out = SensorReading::make(0, _id, getType(),
                                          "pm25", _pm25, "ug/m3");
                return true;
            }
            pos = 0; // bad frame, keep scanning
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
int SDS011Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_serial || !_enabled || maxOut < 2) return 0;

    uint8_t buf[FRAME_LEN];
    int     pos = 0;
    unsigned long deadline = millis() + 1500;

    while (millis() < deadline) {
        if (!_serial->available()) { delay(5); continue; }
        uint8_t b = _serial->read();
        if (pos == 0 && b != 0xAA) continue;
        buf[pos++] = b;
        if (pos == FRAME_LEN) {
            if (_parseFrame(buf)) {
                out[0] = SensorReading::make(0, _id, getType(),
                                              "pm25", _calPm25.apply(_pm25), "ug/m3");
                out[1] = SensorReading::make(0, _id, getType(),
                                              "pm10", _calPm10.apply(_pm10), "ug/m3");
                _newData    = false;
                _lastReadTs = 0;
                return 2;
            }
            pos = 0;
        }
    }
    return 0;
}
