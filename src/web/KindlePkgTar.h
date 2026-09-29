// ============================================================================
// src/web/KindlePkgTar.h
//
// Is this upload a Kindle dashboard package? — docs/KINDLE_UPDATE.md §1.
//
// The collector keeps one copy of the Kindle extension (kindle/ as the build
// packs it, tools/mk_kindle_package.sh) on its SD card and hands it to every
// reader whose version differs. The reader unpacks it AS ROOT over the folder
// it is running from, so what is accepted here is narrow on purpose:
//
//   * a POSIX ustar archive, uncompressed — the collector has to read every
//     header on the way past, and inflating on an ESP32 costs 40 KB of heap
//     that a 400 KB file on an SD card does not justify;
//   * regular files and directories only. No links of either kind, no
//     devices, no GNU long-name or pax records: a symlink in a tarball that
//     root unpacks is a write to wherever it points;
//   * every path under esp32dash/, made of [A-Za-z0-9._-] components, none of
//     them "." or "..";
//   * none of the files that belong to the READER, not to the package —
//     dash.conf, the collector scan list, the log and the cached page. The
//     reader would otherwise have its settings replaced by an update;
//   * the files the extension cannot run without, and a VERSION whose first
//     line is the version the collector offers and the reader compares.
//
// The reader checks all of it again before it touches its own folder
// (update_dash.sh, pkg_install): the collector refusing a file is what stops
// the wrong upload from being offered at all, not what makes the install safe.
//
// Streaming, like nodefw::MarkerScan: fed the upload a segment at a time, in
// any split, with no allocation — 512 bytes of header and a few flags.
//
// Header-only and free of Arduino, so tests/host/test_kindle_pkg.cpp runs it.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>   // strcasecmp

namespace kpkg {

/// The largest package taken. The extension is ~400 KB unpacked today.
static constexpr uint32_t MAX_SIZE = 1536u * 1024u;

/// VERSION's first line, NUL included: `git describe --tags --always --dirty`
/// is well under this.
static constexpr size_t VER_CAP = 48;

/// Every path starts with this.
static constexpr char ROOT[] = "esp32dash/";

/// The files the extension cannot run without (§1).
static constexpr const char* const REQUIRED[] = {
    "esp32dash/update_dash.sh",
    "esp32dash/start.sh",
    "esp32dash/stop.sh",
    "esp32dash/menu.json",
    "esp32dash/VERSION",
};
static constexpr int REQUIRED_N = sizeof(REQUIRED) / sizeof(REQUIRED[0]);

/// The reader's own files: never in a package.
static constexpr const char* const READER_OWN[] = {
    "esp32dash/dash.conf",
    "esp32dash/collectors",
    "esp32dash/kual.log",
    "esp32dash/last.txt",
};

/// A version is shown to people and sent back in a query string, so it is
/// held to what `git describe` produces.
inline bool verCharOk(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '+';
}

inline bool verOk(const char* v) {
    if (!v || !*v) return false;
    size_t n = 0;
    for (; v[n]; n++)
        if (n + 1 >= VER_CAP || !verCharOk(v[n])) return false;
    return true;
}

inline bool pathCharOk(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == '/';
}

/// `p` is a path the reader may write: under ROOT, known characters, no
/// empty, "." or ".." component. A directory's trailing '/' is allowed.
inline bool pathOk(const char* p) {
    const size_t rn = sizeof(ROOT) - 1;
    if (strncmp(p, ROOT, rn) != 0) return false;
    for (const char* c = p; *c; c++)
        if (!pathCharOk(*c)) return false;
    const char* s = p + rn;
    while (*s) {
        const char* e = strchr(s, '/');
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0) return false;                               // "a//b"
        if (n == 1 && s[0] == '.') return false;
        if (n == 2 && s[0] == '.' && s[1] == '.') return false;
        if (!e) break;
        s = e + 1;                                              // "x/" ends here
    }
    return true;
}

/// An octal field as tar writes it: digits, then NUL or space padding.
inline bool octal(const uint8_t* f, size_t n, uint32_t& out) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && f[i] == ' ') i++;
    bool any = false;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        v = v * 8 + (f[i] - '0');
        if (v > 0xFFFFFFFFu) return false;
        any = true;
    }
    for (; i < n; i++)
        if (f[i] != ' ' && f[i] != '\0') return false;
    out = (uint32_t)v;
    return any;
}

enum Phase : uint8_t { PH_HEADER, PH_DATA, PH_PAD, PH_END };

struct TarScan {
    uint8_t     hdr[512];
    uint16_t    fill;           ///< bytes of hdr (or of a zero block) so far
    Phase       phase;
    uint32_t    remain;         ///< data bytes left in this entry
    uint32_t    pad;            ///< padding bytes left after them
    uint8_t     zeros;          ///< consecutive all-zero header blocks
    bool        inVersion;      ///< this entry is esp32dash/VERSION
    bool        verLineDone;
    uint8_t     vlen;
    char        ver[VER_CAP];
    uint32_t    have;           ///< bit i = REQUIRED[i] seen
    uint32_t    total;
    uint16_t    entries;
    const char* err;            ///< first refusal, or nullptr
};

inline void scanBegin(TarScan& s) { memset(&s, 0, sizeof(s)); }

inline void fail(TarScan& s, const char* e) { if (!s.err) s.err = e; }

inline bool allZero(const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (b[i]) return false;
    return true;
}

/// One complete 512-byte header block.
inline void header(TarScan& s) {
    const uint8_t* h = s.hdr;
    if (allZero(h, 512)) {
        // Two in a row end the archive; everything after is record padding.
        if (++s.zeros >= 2) s.phase = PH_END;
        return;
    }
    if (s.zeros) { fail(s, "not_package"); return; }   // one zero block, then more

    // The magic first: an upload of the .bin or the .zip fails here, and
    // "that is not the package" is the useful thing to say about it.
    if (memcmp(h + 257, "ustar", 5) != 0) { fail(s, "not_package"); return; }

    uint32_t sum = 0, want = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (!octal(h + 148, 8, want) || want != sum) { fail(s, "not_package"); return; }

    // prefix "/" name, each NUL-terminated only when shorter than its field.
    char path[256 + 2];
    size_t pn = 0;
    const size_t prefLen = strnlen((const char*)h + 345, 155);
    if (prefLen) {
        memcpy(path, h + 345, prefLen);
        pn = prefLen;
        path[pn++] = '/';
    }
    const size_t nameLen = strnlen((const char*)h, 100);
    memcpy(path + pn, h, nameLen);
    pn += nameLen;
    path[pn] = '\0';

    uint32_t size = 0;
    if (!octal(h + 124, 12, size)) { fail(s, "not_package"); return; }

    const char type = (char)h[156];
    if (type == '5') {
        if (size != 0) { fail(s, "bad_entry"); return; }
        // "esp32dash/" itself is the one directory that is ROOT exactly.
        if (strcmp(path, ROOT) != 0 && !pathOk(path)) { fail(s, "bad_path"); return; }
    } else if (type == '0' || type == '\0') {
        if (!pathOk(path) || path[pn - 1] == '/') { fail(s, "bad_path"); return; }
        // WITHOUT REGARD TO CASE: the reader's folder is on FAT, where
        // esp32dash/DASH.CONF is the same file as its dash.conf.
        for (const char* own : READER_OWN)
            if (strcasecmp(path, own) == 0) { fail(s, "bad_path"); return; }
        for (int i = 0; i < REQUIRED_N; i++)
            if (strcmp(path, REQUIRED[i]) == 0) s.have |= 1u << i;
    } else {
        // Links, devices, fifos, GNU 'L'/'K' and pax 'x'/'g' records.
        fail(s, "bad_entry");
        return;
    }
    s.entries++;
    s.inVersion   = strcmp(path, "esp32dash/VERSION") == 0;
    if (s.inVersion) { s.vlen = 0; s.verLineDone = false; s.ver[0] = '\0'; }
    s.remain = size;
    s.pad    = (512 - (size % 512)) % 512;
    s.phase  = size ? PH_DATA : PH_HEADER;
}

/// Feed the next `n` bytes of the upload.
inline void scanFeed(TarScan& s, const uint8_t* p, size_t n) {
    if (s.total + (uint64_t)n > MAX_SIZE) { fail(s, "too_big"); }
    s.total += (uint32_t)n;
    while (n && !s.err) {
        switch (s.phase) {
        case PH_HEADER: {
            size_t k = 512 - s.fill;
            if (k > n) k = n;
            memcpy(s.hdr + s.fill, p, k);
            s.fill += (uint16_t)k;
            p += k; n -= k;
            if (s.fill == 512) { s.fill = 0; header(s); }
            break;
        }
        case PH_DATA: {
            size_t k = s.remain < n ? s.remain : n;
            if (s.inVersion && !s.verLineDone) {
                for (size_t i = 0; i < k; i++) {
                    const char c = (char)p[i];
                    if (c == '\n' || c == '\r') { s.verLineDone = true; break; }
                    if ((size_t)s.vlen + 1 >= VER_CAP || !verCharOk(c)) {
                        fail(s, "bad_version");
                        break;
                    }
                    s.ver[s.vlen++] = c;
                    s.ver[s.vlen] = '\0';
                }
            }
            s.remain -= (uint32_t)k;
            p += k; n -= k;
            if (!s.remain) s.phase = s.pad ? PH_PAD : PH_HEADER;
            break;
        }
        case PH_PAD: {
            size_t k = s.pad < n ? s.pad : n;
            s.pad -= (uint32_t)k;
            p += k; n -= k;
            if (!s.pad) s.phase = PH_HEADER;
            break;
        }
        case PH_END:
            // Record padding: zeros, and nothing else.
            if (!allZero(p, n)) fail(s, "not_package");
            n = 0;
            break;
        }
    }
}

/// After the last byte: nullptr and `ver` filled when this is a package,
/// otherwise the reason — the API's `error` string.
inline const char* scanFinish(TarScan& s, char* ver, size_t verCap) {
    if (ver && verCap) ver[0] = '\0';
    if (s.err) return s.err;
    if (s.phase != PH_END) return s.total ? "truncated" : "not_package";
    for (int i = 0; i < REQUIRED_N; i++)
        if (!(s.have & (1u << i))) return "missing_files";
    if (!verOk(s.ver)) return "bad_version";
    if (ver && verCap) {
        strncpy(ver, s.ver, verCap - 1);
        ver[verCap - 1] = '\0';
    }
    return nullptr;
}

}  // namespace kpkg
