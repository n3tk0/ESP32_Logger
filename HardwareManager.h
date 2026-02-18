#pragma once
#include <Arduino.h>

void initHardware();
void debounceButton(uint8_t pin, int& last, int& stable,
                    unsigned long& lastTime, int& count);

void IRAM_ATTR onFFButton();
void IRAM_ATTR onPFButton();
void IRAM_ATTR onFlowPulse();
