// The RingBuffer tests again, against the compact storage the ESP32-C3
// builds use (12 B per reading + a key table, see RING_COMPACT in
// src/pipeline/DataPipeline.h). Same API, so the same expectations hold.
#define RING_COMPACT 1
#include "test_ringbuffer.cpp"
