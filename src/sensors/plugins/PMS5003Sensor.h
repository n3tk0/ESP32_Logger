#pragma once
#include "../ISensor.h"

// ============================================================================
// PMS5003 — PM1.0 / PM2.5 / PM10 (UART, Plantower protocol)
// Config keys: "uart_rx", "uart_tx", "baud" (9600)
// Produces 3 metrics: pm1, pm25, pm10  (ug/m3, atmospheric concentrations)
// ============================================================================
class PMS5003Sensor : public ISensor {
public:
    bool init(JsonObjectConst cfg) override;
    bool read(SensorReading& out) override;
    int  readAll(SensorReading* out, int maxOut) override;

    const char* getType() const override { return "pms5003"; }
    const char* getName() const override { return "PMS5003 PM1/2.5/10"; }
    uint32_t    getReadIntervalMs() const override { return _intervalMs; }
    bool        isContinuous() const override { return true; }
    bool        isBlocking()   const override { return true; }
    int getMetrics(const char** out, int maxOut) const override {
        static const char* m[] = { "pm1", "pm25", "pm10" };
        int n = 3; if (n > maxOut) n = maxOut;
        for (int i = 0; i < n; i++) out[i] = m[i];
        return n;
    }

private:
    HardwareSerial* _serial     = nullptr;
    uint32_t        _intervalMs = 30000;
    float           _pm1 = 0, _pm25 = 0, _pm10 = 0;

    CalibrationAxis _calPm1;
    CalibrationAxis _calPm25;
    CalibrationAxis _calPm10;

    static constexpr uint8_t  START1    = 0x42;
    static constexpr uint8_t  START2    = 0x4D;
    static constexpr int      FRAME_LEN = 32;

    bool _readFrame();
};
