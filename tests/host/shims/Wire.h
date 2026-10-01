#pragma once
// ----------------------------------------------------------------------------
// Host shim for <Wire.h>: one I2C device backed by a 256-byte register file.
//
// A write transaction's first byte sets the register pointer and every byte
// after it is stored there, auto-incrementing; requestFrom() reads from the
// pointer onward. That is how the Bosch environmental sensors behave on I2C,
// which is all the drivers under test need. A transaction addressed anywhere
// but `addr` is NACKed, so "not found" paths can be driven too.
// ----------------------------------------------------------------------------
#include <Arduino.h>

class TwoWire {
public:
    uint8_t addr = 0x76;
    uint8_t regs[256] = {};

    void   beginTransmission(uint8_t a) { _a = a; _first = true; }
    size_t write(uint8_t b) {
        if (_first) { _ptr = b; _first = false; }
        else        { regs[_ptr++] = b; }
        return 1;
    }
    uint8_t endTransmission(bool = true) { return _a == addr ? 0 : 2; }
    uint8_t requestFrom(uint8_t a, uint8_t len) {
        if (a != addr) { _avail = 0; return 0; }
        _rd = _ptr; _avail = len; _ptr = (uint8_t)(_ptr + len);
        return len;
    }
    int available() { return _avail; }
    int read() {
        if (!_avail) return -1;
        _avail--;
        return regs[_rd++];
    }

private:
    uint8_t _a = 0, _ptr = 0, _rd = 0;
    bool    _first = true;
    int     _avail = 0;
};

// The default bus a driver's begin() names. Never driven by a test.
inline TwoWire Wire;
