#pragma once
// src/web/BlockBuf.h
//
// A reply assembled in 512 B blocks rather than one buffer that grows. The
// Kindle payload is written a byte at a time; in a growing AsyncResponseStream
// every byte past the first 1460 was a realloc of the whole buffer (old and
// new held at once), and on a fragmented C3 heap the growth failed and the
// payload went out cut short. Small blocks fit the gaps a fragmented heap
// has, the length is known before the first byte is sent, and read() frees
// each block as soon as it has gone out.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>

// Where blocks come from. The firmware installs a fixed pool (ReplyPool.cpp)
// so replies do not compete with the rest of the heap; alloc() returns
// nullptr when the pool is empty and the block comes from the heap instead,
// and release() returns false for a block that is not the pool's. Unset (the
// host tests), every block is the heap's.
struct BlockBufSource {
    void* (*alloc)()          = nullptr;
    bool  (*release)(void* p) = nullptr;
};
inline BlockBufSource& blockBufSource() { static BlockBufSource s; return s; }

class BlockBuf {
public:
    static constexpr size_t BLOCK = 512;
    struct Blk { Blk* next; size_t len; uint8_t data[BLOCK]; };
    static constexpr size_t BLK_BYTES = sizeof(Blk);

    BlockBuf() = default;
    BlockBuf(const BlockBuf&) = delete;
    BlockBuf& operator=(const BlockBuf&) = delete;
    ~BlockBuf() {
        while (_head) { Blk* n = _head->next; freeBlk(_head); _head = n; }
    }

    // All of it, or nothing more from here on: once a block cannot be had
    // the reply is incomplete, and failed() says so.
    size_t write(const uint8_t* p, size_t n) {
        if (_failed) return 0;
        size_t done = 0;
        while (done < n) {
            if (!_tail || _tail->len == BLOCK) {
                Blk* b = allocBlk();
                if (!b) { _failed = true; return done; }
                b->next = nullptr;
                b->len  = 0;
                if (_tail) _tail->next = b; else _head = b;
                _tail = b;
            }
            const size_t k = n - done < BLOCK - _tail->len ? n - done : BLOCK - _tail->len;
            memcpy(_tail->data + _tail->len, p + done, k);
            _tail->len += k;
            done  += k;
            _size += k;
        }
        return done;
    }

    bool   failed() const { return _failed; }
    size_t size()   const { return _size; }

    // The next bytes, in order; 0 at the end. Reading starts once writing is
    // over (the reply's length is sent first), so each block is freed as
    // soon as it has been read.
    size_t read(uint8_t* dst, size_t max) {
        size_t got = 0;
        while (got < max && _head) {
            const size_t k = _head->len - _off < max - got ? _head->len - _off : max - got;
            memcpy(dst + got, _head->data + _off, k);
            got  += k;
            _off += k;
            if (_off == _head->len) {
                Blk* n = _head->next;
                if (_head == _tail) _tail = nullptr;
                freeBlk(_head);
                _head = n;
                _off  = 0;
            }
        }
        return got;
    }

private:
    static Blk* allocBlk() {
        const BlockBufSource& src = blockBufSource();
        void* m = src.alloc ? src.alloc() : nullptr;
        if (!m) m = ::operator new(sizeof(Blk), std::nothrow);
        return static_cast<Blk*>(m);
    }
    static void freeBlk(Blk* b) {
        const BlockBufSource& src = blockBufSource();
        if (src.release && src.release(b)) return;
        ::operator delete(b);
    }

    Blk*   _head = nullptr;
    Blk*   _tail = nullptr;
    size_t _off  = 0;
    size_t _size = 0;
    bool   _failed = false;
};
