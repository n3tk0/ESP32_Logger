#pragma once
#include "../ISensor.h"

// ============================================================================
// SDS011 — PM2.5 / PM10 laser dust sensor (UART)
// Based on Airrohr / sensors-software driver pattern.
// Config keys: "uart_rx", "uart_tx" (-1 = TX not connected),
//              "baud" (9600), "work_period_min" (0=continuous)
// Produces 2 metrics: pm25, pm10  (ug/m3)
//
// Work period modes:
//   0 (continuous): sensor streams ~1 frame/s.  readAll() blocks up to 2s.
//   1-30 (periodic): hardware wakes fan every N min, sends one frame after
//       ~30s warm-up.  readAll() does a non-blocking drain of the UART buffer
//       and is polled every 1s by SlowSensorTask.  incErrorCount() is
//       suppressed so the "no data yet" polls don't flag the sensor as broken.
// ============================================================================
class SDS011Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "sds011"; }
    const char* getName() const override { return "SDS011 PM2.5/PM10"; }
    uint32_t    getReadIntervalMs() const override { return _readIntervalMs; }
    bool        isContinuous() const override { return true; }
    bool        isBlocking()   const override { return true; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "pm25", "pm10" };
        int n = 2; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

    void incErrorCount() override {
        // In periodic mode, empty reads are expected (sensor only sends once
        // per period).  Only count errors in continuous mode where every poll
        // should produce data.
        if (_hwPeriodMin == 0) ISensor::incErrorCount();
    }

private:
    HardwareSerial* _serial         = nullptr;
    uint8_t         _hwPeriodMin    = 1;
    uint32_t        _readIntervalMs = 1000;
    float           _pm25           = 0.0f;
    float           _pm10           = 0.0f;

    CalibrationAxis _calPm25;
    CalibrationAxis _calPm10;

    uint8_t         _frameBuf[10] = {0};
    int             _framePos     = 0;

    static constexpr int SERIAL_NUM = 2;
    static constexpr int FRAME_LEN  = 10;

    bool _parseFrame(const uint8_t* buf);
    void _drainBuffer();
    int  _tryReadFrame(SensorReading* out, int maxOut);
};
