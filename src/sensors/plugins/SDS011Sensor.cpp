#include "SDS011Sensor.h"
#include "../../pipeline/DataPipeline.h"

// ---------------------------------------------------------------------------
bool SDS011Sensor::init(JsonObjectConst cfg) {
    _enabled = cfg["enabled"] | true;

    int rxPin = cfg["uart_rx"] | 20;
    int txPin = cfg["uart_tx"] | -1;
    int baud  = cfg["baud"]    | 9600;
    int work  = cfg["work_period_min"] | 1;
    if (work > 30) work = 30;
    if (work < 0) work = 0;

    _hwPeriodMin = (uint8_t)work;

    // Continuous mode: sensor streams ~1 frame/s → poll every 1s with 2s block.
    // Periodic mode: sensor sends one frame every N min → poll every 1s,
    //   non-blocking drain of UART buffer so we catch the frame promptly.
    _readIntervalMs = 1000;

    JsonObjectConst cal = cfg["calibration"];
    _calPm25.load(cal, "pm25");
    _calPm10.load(cal, "pm10");

    _serial = &Serial1;
    if (txPin >= 0) {
        _serial->begin(baud, SERIAL_8N1, rxPin, txPin);
    } else {
        _serial->begin(baud, SERIAL_8N1, rxPin, -1);
    }

    _drainBuffer();

    // Configure SDS011 Hardware Working Period
    if (txPin >= 0) {
        uint8_t cmd[19] = {0xAA, 0xB4, 0x08, 0x01, (uint8_t)work, 0,0,0,0,0,0,0,0,0,0, 0xFF, 0xFF, 0, 0xAB};
        uint8_t sum = 0;
        for (int i = 2; i <= 16; i++) sum += cmd[i];
        cmd[17] = sum;
        _serial->write(cmd, 19);
        _serial->flush();
        delay(100);
        _drainBuffer();
    }

    Serial.printf("[SDS011] RX=%d TX=%d baud=%d period=%umin mode=%s\n",
                  rxPin, txPin, baud, _hwPeriodMin,
                  _hwPeriodMin == 0 ? "continuous" : "hw-periodic");
    return true;
}

// ---------------------------------------------------------------------------
bool SDS011Sensor::_parseFrame(const uint8_t* buf) {
    if (buf[0] != 0xAA || buf[1] != 0xC0 || buf[9] != 0xAB) return false;

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
// Non-blocking frame scan: drains all available bytes looking for a valid
// frame.  Returns 2 on success (pm25 + pm10), 0 if no complete frame yet.
// ---------------------------------------------------------------------------
int SDS011Sensor::_tryReadFrame(SensorReading* out, int maxOut) {
    if (!_serial || maxOut < 2) return 0;

    while (_serial->available()) {
        uint8_t b = _serial->read();
        if (_framePos == 0 && b != 0xAA) continue;
        _frameBuf[_framePos++] = b;
        if (_framePos == FRAME_LEN) {
            if (_parseFrame(_frameBuf)) {
                out[0] = SensorReading::make(0, _id, getType(),
                                              "pm25", _calPm25.apply(_pm25), "ug/m3");
                out[1] = SensorReading::make(0, _id, getType(),
                                              "pm10", _calPm10.apply(_pm10), "ug/m3");
                _framePos = 0;
                return 2;
            }
            // Invalid frame, reset
            _framePos = 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
bool SDS011Sensor::read(SensorReading& out) {
    if (!_serial || !_enabled) return false;
    SensorReading tmp[2];
    if (_tryReadFrame(tmp, 2) > 0) {
        out = tmp[0];
        return true;
    }
    // Continuous mode: block briefly if no data yet
    if (_hwPeriodMin == 0) {
        unsigned long deadline = millis() + 2000;
        while (millis() < deadline) {
            g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();
            if (!_serial->available()) { delay(5); continue; }
            if (_tryReadFrame(tmp, 2) > 0) {
                out = tmp[0];
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
int SDS011Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_serial || !_enabled || maxOut < 2) return 0;

    // Hardware periodic mode: non-blocking drain of UART buffer.
    // Called every ~1s by SlowSensorTask — catches the frame within 1s of
    // transmission without blocking the task.
    if (_hwPeriodMin > 0) {
        return _tryReadFrame(out, maxOut);
    }

    // Continuous mode (work_period=0): sensor sends ~1 frame/s.
    // Block up to 2s to guarantee catching the next frame.
    int n = _tryReadFrame(out, maxOut);
    if (n > 0) return n;

    unsigned long deadline = millis() + 2000;
    while (millis() < deadline) {
        g_taskHeartbeat[TASK_IDX_SLOW_SENSOR] = millis();
        if (!_serial->available()) { delay(5); continue; }
        n = _tryReadFrame(out, maxOut);
        if (n > 0) return n;
    }
    return 0;
}
