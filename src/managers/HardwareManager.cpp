#include "HardwareManager.h"
#include "../core/Globals.h"
#include "RtcManager.h"

// ============================================================================
// ISR HANDLERS
// ============================================================================
// R12 / AUDIT 1.5: onFFButton + onPFButton removed.  They were never
// attachInterrupt'd anywhere; the ffPressed / pfPressed flags they
// touched were write-only.  Buttons run via polled debounceButton().
// R28 / AUDIT 8.11: ISR_DEBOUNCE_MICROS=1000 caps pulse rate at ~1 kHz.
//   YF-S201 (~450 pulses/L) → max measurable ~133 L/min.
//   YF-S403 (~600 pulses/L) → max measurable ~100 L/min.
// Residential use is well below these; bump ISR_DEBOUNCE_MICROS in setup.h
// only if a higher-flow meter (or low-PPL meter) is wired up.
void IRAM_ATTR onFlowPulse() {
    unsigned long now = micros();
    if (now - lastFlowInterrupt > ISR_DEBOUNCE_MICROS) {
        // R28 / AUDIT 2.13: relaxed fetch_add — increment is independent of
        // surrounding loads; loop() exchange(0) provides the read-modify-write
        // barrier when consuming the count.
        pulseCount.fetch_add(1, std::memory_order_relaxed);
        lastFlowInterrupt = now;
        flowSensorPulseDetected = true;
    }
}

// ============================================================================
// DEBOUNCE BUTTON (polling)
// ============================================================================
void debounceButton(uint8_t pin, int& last, int& stable,
                    unsigned long& lastTime, int& count) {
    // Seed lastTime on first call so the first edge doesn't bypass debounce.
    // Globals initialises lastTime to 0; treat 0 as uninitialised.
    if (lastTime == 0) lastTime = millis();
    int reading = digitalRead(pin);
    if (reading != last) { lastTime = millis(); last = reading; }

    if ((millis() - lastTime) > config.hardware.debounceMs && reading != stable) {
        int prev = stable;
        stable   = reading;
        int expectedActive   = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
        int expectedInactive = (expectedActive == HIGH) ? LOW : HIGH;
        if (prev == expectedInactive && stable == expectedActive) count++;
    }
}

// ============================================================================
// HARDWARE INIT
// ============================================================================
void initHardware() {
    DBGLN("Init hardware...");

    // PIN_UNSET (0xFF) and out-of-range / strap-bus pins must never reach
    // pinMode() — on a fresh device (first-run wizard) every pin is PIN_UNSET,
    // and pinMode() on an invalid GPIO is at best a silent no-op and at worst
    // an abort. Skip any pin that isn't a usable user GPIO.
    auto isPinSafe = [](int p) {
        if (p < 0 || p > 21) return false;
        if (p >= 11 && p <= 17) return false;
        return true;
    };

    // Setup button pins
    uint8_t mode = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH)
                       ? INPUT_PULLDOWN : INPUT_PULLUP;
    if (isPinSafe(config.hardware.pinWakeupFF))    pinMode(config.hardware.pinWakeupFF,    mode);
    if (isPinSafe(config.hardware.pinWakeupPF))    pinMode(config.hardware.pinWakeupPF,    mode);
    if (isPinSafe(config.hardware.pinWifiTrigger)) pinMode(config.hardware.pinWifiTrigger, mode);
    // R12 / AUDIT 1.3: INPUT_PULLUP for YF-S201 — the sensor is an open-
    // collector hall effect and needs the internal pull-up to be readable.
    if (isPinSafe(config.hardware.pinFlowSensor))  pinMode(config.hardware.pinFlowSensor,  INPUT_PULLUP);

    // Init RTC
    initRtc();

    DBGLN("Hardware init complete");
}
