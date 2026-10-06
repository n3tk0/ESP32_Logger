// ============================================================================
// src/utils/Utf8Clip.h
//
// A name copied into a fixed buffer is cut at a byte, and a Cyrillic letter is
// two: "Външен сензор" in a sensor id's 16 bytes ends half way through "е".
// The firmware matched that half letter consistently, but a browser cannot
// hold it — JSON.parse turns it into U+FFFD, the page saves that back, and the
// buffer cut THAT to one byte (0xEF), so the saved id no longer matched the
// readings' one and the data log column stayed empty.
//
// utf8ClipTail() drops a sequence left incomplete at the end of the string,
// so every clipped name is valid UTF-8 and survives the round trip. The data
// log keeps its columns clipped and compares the readings' id clipped the same
// way (Datalog.cpp), so a list saved with the broken byte still matches. The
// page keys its column table the same way (dlClip() in www/js/settings.js).
//
// Pure, so host tests compile it directly. Out of line on purpose: one copy
// for every caller, the C3 image has no room for several.
// ============================================================================
#pragma once

#include <stddef.h>
#include <string.h>

__attribute__((noinline)) inline void utf8ClipTail(char* s) {
    size_t n = strlen(s), i = n;
    while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;   // continuation bytes
    if (!i) return;
    const unsigned char lead = (unsigned char)s[i - 1];
    if (lead < 0xC0) return;            // ASCII, or stray continuations: leave it
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (n - (i - 1) < need) s[i - 1] = '\0';
}
