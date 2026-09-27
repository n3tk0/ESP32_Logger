// ============================================================================
// src/nodecfg/FwImage.h — what makes a .bin a node firmware image
//
// docs/NODE_OTA.md §1. Shared by the collector (checking an upload before it
// is offered to any node), the ESP-NOW node (checking what it downloaded
// before it switches to it) and both nodes' own pages (checking a local
// upload). One implementation, so the three can never disagree about what a
// node image is.
//
// THE MARKER. Every node firmware carries one string in its image:
//
//     NODEFW1|<kind>|<version>|
//
// where <kind> is "esp8266" (node/) or "espnow-c3" (node_espnow/). Nothing in
// an ESP image header says which of OUR firmwares it is — the collector, the
// ESP-NOW node and an unrelated sketch are all 0xE9 images, and on Arduino
// core 2.x the app descriptor's project name is the lib builder's for every
// sketch — so the build states it itself. An image without a marker is not a
// node image; one with the wrong kind is refused before a byte is written.
//
// The scanner below does not contain the prefix as a plain string (it is
// stored XOR-ed): otherwise every firmware that links it — the collector
// included — would carry something that looks like a marker to itself.
//
// Header-only and dependency-free, so tests/host reaches it.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/// The marker text for a firmware of `kind` at `ver` (both string literals).
/// A firmware defines it once and must REFERENCE it (print it at boot):
/// --gc-sections drops an unreferenced array, marker and all.
#define NODEFW_MARKER_TEXT(kind, ver) "NODEFW1|" kind "|" ver "|"

namespace nodefw {

enum Kind : uint8_t {
    KIND_NONE      = 0,
    KIND_ESP8266   = 1,   ///< node/ — the WiFi node
    KIND_ESPNOW_C3 = 2,   ///< node_espnow/ — the ESP-NOW node
};

static const char* const KIND_NAMES[] = { "", "esp8266", "espnow-c3" };

inline const char* kindName(Kind k) {
    return (unsigned)k < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]) ? KIND_NAMES[k] : "";
}

inline Kind kindFromName(const char* s) {
    if (!s) return KIND_NONE;
    if (strcmp(s, "esp8266") == 0) return KIND_ESP8266;
    if (strcmp(s, "espnow-c3") == 0) return KIND_ESPNOW_C3;
    return KIND_NONE;
}

/// Largest image of each kind: the slot it has to fit. The ESP8266's sketch
/// space on eagle.flash.4m1m is 1 MB minus a sector; the C3 node's OTA slot
/// is 1280 KB (the board's default partition table).
inline uint32_t maxSize(Kind k) {
    switch (k) {
        case KIND_ESP8266:   return 0xFF000;
        case KIND_ESPNOW_C3: return 0x140000;
        default:             return 0;
    }
}

static const size_t VER_CAP  = 24;   ///< version text, terminator included
static const size_t BODY_CAP = 40;   ///< "<kind>|<version>|", terminator included

/// Streaming search for the marker: feed the image in any size of chunks, in
/// order. Finds a marker split across two chunks.
struct MarkerScan {
    uint8_t matched;          ///< prefix bytes matched so far
    bool    inBody;           ///< collecting "<kind>|<version>|"
    uint8_t bodyLen;
    char    body[BODY_CAP];
    Kind    kind;             ///< first valid marker's kind, KIND_NONE = none yet
    char    ver[VER_CAP];
    bool    conflict;         ///< a second valid marker named another kind
};

namespace detail {
/// "NODEFW1|" XOR 0x5A. See the file comment for why it is not a literal.
static const uint8_t PREFIX_X[8] = { 0x14, 0x15, 0x1E, 0x1F, 0x1C, 0x0D, 0x6B, 0x26 };
static const uint8_t PREFIX_KEY  = 0x5A;
static const uint8_t PREFIX_LEN  = 8;

inline uint8_t prefixAt(uint8_t i) { return (uint8_t)(PREFIX_X[i] ^ PREFIX_KEY); }

inline bool verCharOk(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '.' || c == '-' || c == '_' || c == '+';
}

/// "<kind>|<version>|" → kind + version. False for anything else.
inline bool parseBody(const char* b, size_t n, Kind& kind, char ver[VER_CAP]) {
    const char* bar = (const char*)memchr(b, '|', n);
    if (!bar) return false;
    const size_t klen = (size_t)(bar - b);
    char k[16];
    if (klen == 0 || klen >= sizeof(k)) return false;
    memcpy(k, b, klen);
    k[klen] = '\0';
    kind = kindFromName(k);
    if (kind == KIND_NONE) return false;
    const char* v = bar + 1;
    const size_t rest = n - klen - 1;
    const char* end = (const char*)memchr(v, '|', rest);
    if (!end) return false;
    const size_t vlen = (size_t)(end - v);
    if (vlen == 0 || vlen >= VER_CAP || end + 1 != b + n) return false;
    for (size_t i = 0; i < vlen; i++)
        if (!verCharOk(v[i])) return false;
    memcpy(ver, v, vlen);
    ver[vlen] = '\0';
    return true;
}
}  // namespace detail

inline void scanBegin(MarkerScan& s) { memset(&s, 0, sizeof(s)); }

inline void scanFeed(MarkerScan& s, const uint8_t* p, size_t n) {
    using namespace detail;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (s.inBody) {
            // The body ends at the NUL the compiler put after the literal.
            if (c == 0) {
                Kind k;
                char v[VER_CAP];
                if (parseBody(s.body, s.bodyLen, k, v)) {
                    if (s.kind == KIND_NONE) {
                        s.kind = k;
                        memcpy(s.ver, v, sizeof(v));
                    } else if (k != s.kind) {
                        s.conflict = true;
                    }
                }
                s.inBody = false;
                s.bodyLen = 0;
            } else if ((size_t)s.bodyLen + 1 < BODY_CAP) {
                s.body[s.bodyLen++] = (char)c;
                continue;
            } else {
                s.inBody = false;   // too long to be ours
                s.bodyLen = 0;
            }
            s.matched = 0;
            continue;
        }
        // "NODEFW1|" has no proper prefix that is also a suffix, so on a
        // mismatch the only possible restart is at this byte itself.
        if (c == prefixAt(s.matched)) {
            if (++s.matched == PREFIX_LEN) {
                s.inBody  = true;
                s.bodyLen = 0;
                s.matched = 0;
            }
        } else {
            s.matched = (c == prefixAt(0)) ? 1 : 0;
        }
    }
}

/// The verdict once the whole image has been fed: the kind of its marker, or
/// KIND_NONE when it has none or two that disagree.
inline Kind scanKind(const MarkerScan& s) { return s.conflict ? KIND_NONE : s.kind; }

// ---------------------------------------------------------------------------
// The image header
// ---------------------------------------------------------------------------

static const uint8_t  ESP_IMAGE_MAGIC   = 0xE9;
/// esp_image_header_t::chip_id for the ESP32-C3.
static const uint16_t CHIP_ID_ESP32C3   = 0x0005;
/// esp_app_desc_t follows the 24-byte image header and the first 8-byte
/// segment header; its magic is its first word.
static const size_t   APP_DESC_OFFSET   = 0x20;
static const uint32_t APP_DESC_MAGIC    = 0xABCD5432;
/// esp_app_desc_t::app_elf_sha256 — the offset esptool's --elf-sha256-offset
/// writes it at (0xB0), 32 bytes.
static const size_t   ELF_SHA_OFFSET    = 0xB0;
/// Bytes of an image's start the checks below need.
static const size_t   HEAD_NEED         = ELF_SHA_OFFSET + 32;

inline uint32_t rdU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/// Does the start of an image look right for `kind`? `head` holds its first
/// `n` bytes (HEAD_NEED is enough for every kind).
inline bool headOk(Kind kind, const uint8_t* head, size_t n) {
    if (!head || n < 1 || head[0] != ESP_IMAGE_MAGIC) return false;
    if (kind == KIND_ESP8266) return true;
    if (kind != KIND_ESPNOW_C3 || n < HEAD_NEED) return false;
    const uint16_t chip = (uint16_t)(head[12] | (head[13] << 8));
    return chip == CHIP_ID_ESP32C3 && rdU32(head + APP_DESC_OFFSET) == APP_DESC_MAGIC;
}

/// The id of a C3 image: the first four bytes of its ELF SHA-256, read little
/// endian — the same number the node computes for the image it runs
/// (memcpy of esp_app_desc_t::app_elf_sha256). Needs HEAD_NEED bytes.
inline uint32_t c3ImageId(const uint8_t* head) { return rdU32(head + ELF_SHA_OFFSET); }

}  // namespace nodefw
