// src/utils/JsonIO.cpp — see JsonIO.h for why these go through one input and
// one output type.
#include "JsonIO.h"

#include <stdlib.h>

namespace {

// The file's bytes in a malloc'd buffer, or nullptr with `err` set. The
// caller frees it.
char* slurp(fs::File& f, size_t maxBytes, size_t& got, DeserializationError& err) {
    got = 0;
    const size_t n = f.size();
    if (n > maxBytes) { err = DeserializationError::NoMemory; return nullptr; }
    if (n == 0)       { err = DeserializationError::EmptyInput; return nullptr; }
    char* buf = static_cast<char*>(malloc(n));
    if (!buf)         { err = DeserializationError::NoMemory; return nullptr; }
    got = f.read(reinterpret_cast<uint8_t*>(buf), n);
    return buf;
}

// Print into a fixed buffer, keeping one byte for the terminator.
struct BufPrint : Print {
    char*  buf;
    size_t cap;
    size_t n = 0;
    BufPrint(char* b, size_t c) : buf(b), cap(c) {}
    size_t write(uint8_t c) override {
        if (n + 1 >= cap) return 0;
        buf[n++] = static_cast<char>(c);
        return 1;
    }
    size_t write(const uint8_t* p, size_t len) override {
        size_t k = 0;
        while (k < len && n + 1 < cap) buf[n++] = static_cast<char>(p[k++]);
        return k;
    }
};

// Print onto a String through a 32-byte buffer, as ArduinoJson's own String
// writer does: the serialiser writes one character at a time, and growing
// the String for each one reallocates far more often.
struct StrPrint : Print {
    String& s;
    char    buf[32];
    size_t  n  = 0;
    bool    ok = true;
    explicit StrPrint(String& out) : s(out) {}
    void drain() {
        if (n && !s.concat(buf, n)) ok = false;
        n = 0;
    }
    size_t write(uint8_t c) override {
        if (n == sizeof(buf)) drain();
        buf[n++] = static_cast<char>(c);
        return 1;
    }
    size_t write(const uint8_t* p, size_t len) override {
        for (size_t k = 0; k < len; k++) write(p[k]);
        return len;
    }
};

} // namespace

DeserializationError deserializeJsonFile(JsonDocument& doc, fs::File& f, size_t maxBytes) {
    size_t got;
    DeserializationError err;
    char* buf = slurp(f, maxBytes, got, err);
    if (!buf) { doc.clear(); return err; }
    err = deserializeJson(doc, static_cast<const char*>(buf), got);
    free(buf);
    return err;
}

DeserializationError deserializeJsonFile(JsonVariant dst, fs::File& f, size_t maxBytes) {
    size_t got;
    DeserializationError err;
    char* buf = slurp(f, maxBytes, got, err);
    if (!buf) return err;
    err = deserializeJson(dst, static_cast<const char*>(buf), got);
    free(buf);
    return err;
}

DeserializationError deserializeJsonFile(JsonDocument& doc, fs::File& f,
                                         const JsonDocument& filter, size_t maxBytes) {
    size_t got;
    DeserializationError err;
    char* buf = slurp(f, maxBytes, got, err);
    if (!buf) { doc.clear(); return err; }
    err = deserializeJson(doc, static_cast<const char*>(buf), got,
                          DeserializationOption::Filter(filter.as<JsonVariantConst>()));
    free(buf);
    return err;
}

size_t jsonToString(JsonVariantConst v, String& out) {
    StrPrint p(out);
    const size_t n = serializeJson(v, static_cast<Print&>(p));
    p.drain();
    return p.ok ? n : 0;
}

size_t jsonToBuf(JsonVariantConst v, char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    BufPrint p(buf, cap);
    serializeJson(v, static_cast<Print&>(p));
    buf[p.n] = '\0';
    return p.n;
}
