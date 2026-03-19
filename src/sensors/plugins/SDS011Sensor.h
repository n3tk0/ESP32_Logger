#pragma once
#include "../ISensor.h"

// ============================================================================
// SDS011 — PM2.5 / PM10 laser dust sensor (UART)
// Based on Airrohr / sensors-software driver pattern.
// Config keys: "uart_rx", "uart_tx" (-1 = TX not connected),
//              "baud" (9600), "work_period_min" (0=continuous)
// Produces 2 metrics: pm25, pm10  (ug/m3)
// ============================================================================
class SDS011Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "sds011"; }
    const char* getName() const override { return "SDS011 PM2.5/PM10"; }
    int getMetrics(const char** m, int max) const override {
        static const char* M[] = {"pm25","pm10"};
        int n = 2; if(n>max) n=max;
        for(int i=0;i<n;i++) m[i]=M[i]; return n;
    }
    uint32_t    getReadIntervalMs() const override { return _workPeriodMs; }
    bool        isContinuous() const override { return true; }

private:
    HardwareSerial* _serial     = nullptr;
    uint32_t        _workPeriodMs = 60000; // 1 minute default
    float           _pm25       = 0.0f;
    float           _pm10       = 0.0f;
    bool            _newData    = false;

    // SDS011 uses UART2 by default on ESP32
    static constexpr int SERIAL_NUM = 2;
    static constexpr int FRAME_LEN  = 10;

    bool _parseFrame(const uint8_t* buf);
    void _drainBuffer();
};
