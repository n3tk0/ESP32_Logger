#pragma once
#include <Arduino.h>

void initHardware();
void debounceButton(uint8_t pin, int& last, int& stable,
                    unsigned long& lastTime, int& count);

// R12 / AUDIT 1.5: onFFButton + onPFButton removed — they were declared
// and defined but never attachInterrupt'd, so the IRAM cost was paid for
// no behaviour. Buttons run via polled debounceButton() instead.
void IRAM_ATTR onFlowPulse();
