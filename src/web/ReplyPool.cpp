// src/web/ReplyPool.cpp — see ReplyPool.h.
#include "ReplyPool.h"
#include "BlockBuf.h"

#include <freertos/FreeRTOS.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
alignas(8) static uint8_t s_pool[REPLY_POOL_BLOCKS][BlockBuf::BLK_BYTES];
static uint16_t s_freeList[REPLY_POOL_BLOCKS];
static uint16_t s_free = 0, s_minFree = 0;
static uint32_t s_heapBlocks = 0;

static void* poolAlloc() {
    void* p = nullptr;
    portENTER_CRITICAL(&s_mux);
    if (s_free) {
        p = s_pool[s_freeList[--s_free]];
        if (s_free < s_minFree) s_minFree = s_free;
    } else {
        s_heapBlocks++;
    }
    portEXIT_CRITICAL(&s_mux);
    return p;
}

static bool poolRelease(void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    const uint8_t* lo = &s_pool[0][0];
    if (b < lo || b >= lo + sizeof(s_pool)) return false;
    portENTER_CRITICAL(&s_mux);
    s_freeList[s_free++] = (uint16_t)((b - lo) / BlockBuf::BLK_BYTES);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void replyPoolBegin() {
    for (uint16_t i = 0; i < REPLY_POOL_BLOCKS; i++) s_freeList[i] = i;
    s_free = s_minFree = REPLY_POOL_BLOCKS;
    BlockBufSource& src = blockBufSource();
    src.alloc   = poolAlloc;
    src.release = poolRelease;
}

ReplyPoolStats replyPoolStats() {
    ReplyPoolStats st;
    portENTER_CRITICAL(&s_mux);
    st.blocks     = REPLY_POOL_BLOCKS;
    st.free       = s_free;
    st.minFree    = s_minFree;
    st.heapBlocks = s_heapBlocks;
    portEXIT_CRITICAL(&s_mux);
    return st;
}
