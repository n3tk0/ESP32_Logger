// The SPSC visibility test against the compact storage (RING_COMPACT=1): the
// key table entry is written before _head is published, so a reader that
// sees a reading also sees its strings.
#define RING_COMPACT 1
#include "test_ringbuffer_concurrency.cpp"
