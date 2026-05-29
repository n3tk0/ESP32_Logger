#pragma once
// ----------------------------------------------------------------------------
// Minimal host shim for <Arduino.h>.
//
// Used ONLY by the desktop unit/fuzz tests (tests/host/*) so that firmware
// headers and a few self-contained .cpp files compile with a normal g++/clang
// toolchain.  It provides:
//   * the C stdlib symbols the pure logic relies on (snprintf, memset, strcmp…)
//   * a small std::string-backed Arduino `String` mimicking the subset of the
//     API the tested code uses
//   * a no-op `Serial` so logging call-sites link
//
// It is NOT a full Arduino emulation and must never be added to the firmware
// include path.
// ----------------------------------------------------------------------------
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include <string>   // host-only: backing store for the String shim

// ----------------------------------------------------------------------------
// Arduino String — std::string-backed, only the methods exercised by the code
// under test.  Semantics match Arduino where it matters (out-of-range
// operator[] returns 0; substring clamps; startsWith is a prefix test).
// ----------------------------------------------------------------------------
class String {
    std::string _s;
public:
    String() {}
    String(const char* s) : _s(s ? s : "") {}
    String(char c) : _s(1, c) {}
    String(const std::string& s) : _s(s) {}

    size_t      length()  const { return _s.size(); }
    bool        isEmpty() const { return _s.empty(); }
    const char* c_str()   const { return _s.c_str(); }
    void        reserve(size_t n) { _s.reserve(n); }

    // Arduino returns 0 for an out-of-range index rather than UB.
    char operator[](int i) const {
        return (i >= 0 && (size_t)i < _s.size()) ? _s[(size_t)i] : 0;
    }

    bool startsWith(const String& p) const {
        return _s.size() >= p._s.size() && _s.compare(0, p._s.size(), p._s) == 0;
    }
    bool startsWith(const char* p) const { return startsWith(String(p)); }

    bool endsWith(const String& p) const {
        return _s.size() >= p._s.size() &&
               _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0;
    }
    bool endsWith(const char* p) const { return endsWith(String(p)); }

    int indexOf(char c, int from = 0) const {
        if (from < 0) from = 0;
        std::string::size_type pos = _s.find(c, (size_t)from);
        return pos == std::string::npos ? -1 : (int)pos;
    }

    // [begin, end) with Arduino-style clamping (and begin/end swap to match
    // Arduino's String::substring when begin > end).
    String substring(int begin, int end) const {
        int len = (int)_s.size();
        if (begin < 0) begin = 0;
        if (end   < 0) end   = 0;
        if (begin > len) begin = len;
        if (end   > len) end   = len;
        if (begin > end) { int t = begin; begin = end; end = t; }
        return String(_s.substr((size_t)begin, (size_t)(end - begin)));
    }
    String substring(int begin) const { return substring(begin, (int)_s.size()); }

    bool operator==(const String& o) const { return _s == o._s; }
    bool operator==(const char* o)   const { return _s == (o ? o : ""); }
    bool operator!=(const String& o) const { return !(*this == o); }
    bool operator!=(const char* o)   const { return !(*this == o); }

    String& operator+=(const String& o) { _s += o._s;            return *this; }
    String& operator+=(const char* o)   { _s += (o ? o : "");    return *this; }
    String& operator+=(char c)          { _s += c;               return *this; }

    friend String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
    friend String operator+(const char*   a, const String& b) { String r(a); r += b; return r; }
    friend String operator+(const String& a, const char*   b) { String r(a); r += b; return r; }
    friend String operator+(const String& a, char          b) { String r(a); r += b; return r; }
};

// ----------------------------------------------------------------------------
// Serial — no-op sink so firmware logging call-sites link on the host.
// ----------------------------------------------------------------------------
class HostSerial {
public:
    int  printf(const char* /*fmt*/, ...) { return 0; }
    void print(const char*)   {}
    void print(const String&) {}
    void println(const char*) {}
    void println(const String&) {}
    void println() {}
    void begin(unsigned long) {}
};
extern HostSerial Serial;   // defined by the test TU that needs it
