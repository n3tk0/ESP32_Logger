#pragma once
// src/web/ReplyPool.h — the blocks web replies are written into (BlockBuf.h),
// set aside at boot so a reply never competes with TLS or the sensor
// pipeline for heap, and never fragments it. When the pool is empty a block
// comes from the heap as before; failing that, the reply is a 503.

#include <stdint.h>

#ifndef REPLY_POOL_BLOCKS
#define REPLY_POOL_BLOCKS 16          // 16 x 520 B, about 8 KB
#endif

/// Installs the pool as BlockBuf's block source. From setup(), once.
void replyPoolBegin();

struct ReplyPoolStats {
    uint16_t blocks, free, minFree;
    uint32_t heapBlocks;              // blocks that had to come from the heap
};
ReplyPoolStats replyPoolStats();
