#pragma once
// Host shim — opaque queue handle type so DataPipeline.h's queue externs
// declare cleanly on the desktop.  The queues themselves are never used by the
// host tests (they only instantiate the header-only RingBuffer template).
typedef void* QueueHandle_t;
