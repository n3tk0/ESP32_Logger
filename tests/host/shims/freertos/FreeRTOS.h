#pragma once
// Host shim for <freertos/FreeRTOS.h> — types only, no RTOS behaviour.
// Lets src/pipeline/DataPipeline.h compile on the desktop so the header-only
// RingBuffer<N> template can be unit-tested.  Not a scheduler emulation.
#include <stdint.h>
