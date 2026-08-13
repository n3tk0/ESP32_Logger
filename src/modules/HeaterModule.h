#pragma once
#include "../core/IModule.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// HeaterModule — closed-loop control for an enclosure heater (Pass 5).
//
// Drives a low-side N-channel MOSFET gate from a PWM pin to hold a sensor
// enclosure above freezing and above the dew point. Built for a PTC element on
// a separate 12 V rail with the logic side on 5 V/3.3 V, which is the usual
// way to keep an SPS30 inside its -10 °C..+60 °C operating range through a
// winter and to keep condensation off its optics.
//
// This is the first actuator in the codebase. Everything else here is
// read-only, and AlertEngine deliberately dispatches only toasts and MQTT
// publishes — it cannot drive a GPIO. A heater is different in kind: it can
// start a fire, so the failure modes drive the design.
//
// CONTROL LAW
// -----------
// Two independent reasons to heat, whichever demands more:
//
//   1. Frost protection — hold the enclosure at `setpointC` (default 5 °C),
//      comfortably inside the SPS30's -10 °C floor without drying the sample
//      air enough to bias the PM readings.
//   2. Condensation protection — hold the enclosure at least `dewMarginC`
//      above the measured dew point. A plain temperature setpoint does not
//      cover this: at +2 °C ambient in fog the enclosure is already above a
//      5 °C-frost-protection threshold's off-band, yet that is exactly when
//      water condenses on cold optics.
//
// The effective target is max(setpointC, dewPoint + dewMarginC), with a
// symmetric hysteresis band of `hysteresisC` around it so the MOSFET is not
// chattered by measurement noise.
//
// SAFETY
// ------
// Every one of these forces the output to 0 %:
//   • module disabled, or no valid pin assigned
//   • enclosure temperature missing or older than `staleTimeoutS`
//     — a dead probe must never leave the heater latched on
//   • enclosure temperature above `maxTempC` (latched until it falls back
//     below maxTempC - OVERTEMP_CLEAR_MARGIN_C)
//   • stop() / reconfiguration
//
// Soft start: a cold PTC has a low resistance and its inrush is several times
// its steady-state draw, which will trip the current limit on a small mains
// module. The duty ramps 0 → target over `softStartMs` on every off→on edge.
//
// This is a software interlock on a software watchdog's schedule; it is not a
// substitute for the hardware ones. The gate still needs its own pull-down so
// the heater is off while the ESP32 is in reset, and the element still needs
// its in-line thermal fuse.
//
// The control loop is driven from ProcessingTask, which ticks at least every
// 100 ms whether or not any sensor is producing data — so the staleness
// fail-safe keeps running precisely when the sensors have stopped.
// ============================================================================
class HeaterModule : public IModule {
public:
    static HeaterModule& instance() { static HeaterModule m; return m; }

    const char* getId()   const override { return "heater"; }
    const char* getName() const override { return "Enclosure heater"; }
    const char* getDescription() const override {
        return "Frost and condensation protection for the sensor enclosure.";
    }

    bool load(JsonObjectConst cfg) override;
    bool save(JsonObject cfg) const override;
    bool start() override;
    void stop()  override;

    // Control loop. Safe to call at any rate; internally rate-limited to
    // CONTROL_PERIOD_MS. MUST keep being called even when no readings arrive.
    void tick(uint32_t nowMs) override;

    void statusJson(JsonObject out) const override;
    const char* schema() const override;

    // Current output duty in percent (0..100) — for diagnostics/tests.
    uint8_t dutyPct() const { return _dutyPct; }

private:
    // Opt-in: never drive a pin unasked. The mutex is created here rather than
    // lazily because instance() is first called from setup(), before any task
    // exists — a lazy create would itself be the race it guards against.
    HeaterModule() {
        _enabled  = false;
        _hwMutex  = xSemaphoreCreateMutex();
    }

    enum Fault : uint8_t {
        FAULT_NONE = 0,
        FAULT_NO_PIN,        // no valid GPIO assigned
        FAULT_STALE_TEMP,    // control input missing or too old
        FAULT_OVERTEMP,      // above maxTempC; latched
    };

    void  _applyDuty(uint8_t pct);
    void  _forceOff(Fault reason);
    // Unconditional, lock-free drive of the gate to its inactive level using
    // plain GPIO. Correct whether or not LEDC currently owns the pin, so it is
    // safe to call from any task at any point — including when the hardware
    // mutex cannot be acquired.
    void  _safeOffNow();
    bool  _attachPin();     // caller must hold _hwMutex
    void  _detachPin();     // caller must hold _hwMutex
    bool  _readInput(const char* sensorId, const char* metric,
                     uint32_t maxAgeMs, float& out) const;
    static const char* _faultText(Fault f);

    // ---- Configuration -----------------------------------------------------
    int      _pin          = -1;
    char     _tempSensor[17] = {};              // enclosure probe (e.g. DS18B20)
    char     _tempMetric[16] = "temperature";
    char     _dewSensor [17] = {};              // dew-point source (e.g. BME680)
    char     _dewMetric [16] = "dew_point";
    float    _setpointC    = 5.0f;
    float    _hysteresisC  = 1.5f;
    float    _dewMarginC   = 2.0f;
    float    _maxTempC     = 45.0f;
    uint32_t _staleTimeoutS = 120;
    uint32_t _softStartMs   = 3000;
    uint32_t _pwmFreqHz     = 200;
    uint8_t  _maxDutyPct    = 100;
    bool     _invert        = false;            // true = active-low gate drive

    // ---- Runtime state -----------------------------------------------------
    // Guards the LEDC attach/detach bookkeeping only. tick() runs on
    // ProcessingTask while stop()/start() are invoked from the AsyncTCP worker
    // via /api/modules/heater/{enable,restart}, so the attach state would
    // otherwise be mutated from two tasks at once.
    SemaphoreHandle_t _hwMutex = nullptr;
    bool     _attached      = false;
    int      _attachedPin   = -1;               // pin currently held by LEDC
    bool     _reconfigure   = true;             // pin/PWM setup pending in tick()
    bool     _heating       = false;
    uint8_t  _dutyPct       = 0;
    uint32_t _rampStartMs   = 0;
    uint32_t _lastControlMs = 0;
    Fault    _fault         = FAULT_NONE;
    bool     _overtempLatch = false;

    // Last resolved control inputs, kept for statusJson.
    float    _lastTempC     = NAN;
    float    _lastDewC      = NAN;
    bool     _dewValid      = false;
    uint32_t _onSinceMs     = 0;

    static constexpr uint32_t CONTROL_PERIOD_MS       = 1000;
    static constexpr float    OVERTEMP_CLEAR_MARGIN_C = 5.0f;
    static constexpr uint8_t  PWM_RESOLUTION_BITS     = 10;   // 0..1023
    static constexpr uint32_t PWM_MAX_DUTY            = (1u << PWM_RESOLUTION_BITS) - 1;
};
