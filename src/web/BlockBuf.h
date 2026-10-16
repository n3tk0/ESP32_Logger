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

class BlockBuf {
public:
    static constexpr size_t BLOCK = 512;

    BlockBuf() = default;
    BlockBuf(const BlockBuf&) = delete;
    BlockBuf& operator=(const BlockBuf&) = delete;
    ~BlockBuf() {
        while (_head) { Blk* n = _head->next; delete _head; _head = n; }
    }

    // All of it, or nothing more from here on: once a block cannot be had
    // the reply is incomplete, and failed() says so.
    size_t write(const uint8_t* p, size_t n) {
        if (_failed) return 0;
        size_t done = 0;
        while (done < n) {
            if (!_tail || _tail->len == BLOCK) {
                Blk* b = new (std::nothrow) Blk;
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
                delete _head;
                _head = n;
                _off  = 0;
            }
        }
        return got;
    }

private:
    struct Blk { Blk* next; size_t len; uint8_t data[BLOCK]; };
    Blk*   _head = nullptr;
    Blk*   _tail = nullptr;
    size_t _off  = 0;
    size_t _size = 0;
    bool   _failed = false;
};
