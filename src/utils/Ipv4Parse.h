// ============================================================================
// src/utils/Ipv4Parse.h
//
// "a.b.c.d" -> four bytes, without sscanf.
//
// WHY NOT sscanf: it was the only thing in this firmware that linked newlib's
// scanf engine (vfscanf.c and friends), roughly 15 KB of flash for two
// dotted-quad parses — the settings form and the WiFi module. The all-features
// build sits within a few KB of the app partition, so that is not small.
//
// It is also stricter, on purpose: "%hhu" wrapped 300 to 44 and "%d" let
// "1.2.3.4junk" through. Here each part is 1-3 digits and at most 255, there
// are exactly four, and nothing may follow but spaces.
//
// Pure, so tests/host/test_ipv4_parse.cpp compiles it directly.
// ============================================================================
#pragma once

#include <stdint.h>

/// True and `out` written when `s` is a dotted quad; false and `out`
/// untouched otherwise (nullptr included). Surrounding spaces are allowed.
inline bool ipv4Parse(const char* s, uint8_t out[4]) {
    if (!s) return false;
    while (*s == ' ') s++;
    uint8_t tmp[4];
    for (int i = 0; i < 4; i++) {
        if (i && *s++ != '.') return false;
        unsigned v = 0;
        int      digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return false;
            v = v * 10u + (unsigned)(*s++ - '0');
        }
        if (!digits || v > 255u) return false;
        tmp[i] = (uint8_t)v;
    }
    while (*s == ' ') s++;
    if (*s) return false;
    for (int i = 0; i < 4; i++) out[i] = tmp[i];
    return true;
}
