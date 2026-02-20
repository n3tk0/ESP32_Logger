#include "HardwareManager.h"
#include "../core/Globals.h"
#include "RtcManager.h"

// ============================================================================
// ISR HANDLERS
// ============================================================================
void IRAM_ATTR onFFButton() {
    unsigned long now = micros();
    if (now - lastFFInterrupt > isrDebounceUs) {
        lastFFInterrupt = now;
        ffPressed = true;
    }
}

void IRAM_ATTR onPFButton() {
    unsigned long now = micros();
    if (now - lastPFInterrupt > isrDebounceUs) {
        lastPFInterrupt = now;
        pfPressed = true;
    }
}

void IRAM_ATTR onFlowPulse() {
    unsigned long now = micros();
    if (now - lastFlowInterrupt > ISR_DEBOUNCE_MICROS) {
        if (Sensor) Sensor->count();
        pulseCount++;
        lastFlowInterrupt = now;
        flowSensorPulseDetected = true;
    }
}

// ============================================================================
// DEBOUNCE BUTTON (polling)
// ============================================================================
void debounceButton(uint8_t pin, int& last, int& stable,
                    unsigned long& lastTime, int& count) {
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

    auto isPinSafe = [](int p) {
        if (p < 0 || p > 21) return false;
        if (p >= 11 && p <= 17) return false;
        return true;
    };

    // Setup button pins
    if (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) {
        pinMode(config.hardware.pinWakeupFF,    INPUT_PULLDOWN);
        pinMode(config.hardware.pinWakeupPF,    INPUT_PULLDOWN);
        pinMode(config.hardware.pinWifiTrigger, INPUT_PULLDOWN);
    } else {
        pinMode(config.hardware.pinWakeupFF,    INPUT_PULLUP);
        pinMode(config.hardware.pinWakeupPF,    INPUT_PULLUP);
        pinMode(config.hardware.pinWifiTrigger, INPUT_PULLUP);
    }
    pinMode(config.hardware.pinFlowSensor, INPUT);

    // Init RTC
    initRtc();

    DBGLN("Hardware init complete");
}
