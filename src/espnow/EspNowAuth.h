// ============================================================================
// src/espnow/EspNowAuth.h
//
// Signing and verifying a DISCOVER frame, in one place both ends compile.
//
// WHY THIS IS NOT IN EspNowProto.h
// --------------------------------
// That header is included by the host tests, which have no mbedTLS and no
// business linking one. This has both ends of the radio as its only callers
// and needs mbedTLS, so it lives separately — and separately means the node
// and the collector still share it, which is the point.
//
// A signature is exactly the kind of thing two implementations get subtly
// different: the region covered, the truncation length, the byte order of the
// nonce. Any of those differing produces a node that pairs with nothing and a
// collector that reports a bad signature, with both sides looking correct in
// isolation. So neither side writes its own — espnowFillDiscover() builds AND
// signs the frame, and espnowVerifyTag() checks exactly what it signed.
//
// WHAT THE TAG BUYS
// -----------------
// It proves the sender holds the shared key. That is what stops a stranger's
// node being adopted by being carried past the house during a pairing window.
//
// It is not privacy: the frame is broadcast, unencrypted and readable by
// anyone in range, and it discloses a MAC address and a node number. And it is
// not replay protection on its own — a captured frame stays valid — which is
// why the collector only listens for DISCOVER while a pairing window is open.
// ============================================================================
#pragma once

#include <mbedtls/md.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "EspNowProto.h"

/// Truncated HMAC-SHA256 over `data`, into `out8`.
///
/// Eight bytes rather than thirty-two because a DISCOVER frame is 22 bytes in
/// total and a full tag would treble it for nothing: 64 bits of forgery
/// resistance is far more than an attacker gets to attempt against a window a
/// human opened and that closes in two minutes.
static inline bool espnowSignTag(const uint8_t* key16, const uint8_t* data,
                                 size_t len, uint8_t out8[8]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || !key16 || !data || !out8) return false;

    uint8_t full[32];
    if (mbedtls_md_hmac(info, key16, 16, data, len, full) != 0) return false;
    memcpy(out8, full, 8);
    return true;
}

/// Constant-time compare of a received tag against the expected one.
///
/// The window is short and the tag is not a password, but a byte-at-a-time
/// memcmp on a value an attacker can iterate is free to get right and awkward
/// to explain otherwise.
static inline bool espnowVerifyTag(const uint8_t* key16, const uint8_t* data,
                                   size_t len, const uint8_t tag[8]) {
    uint8_t want[8];
    if (!espnowSignTag(key16, data, len, want)) return false;

    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    return diff == 0;
}

/// Build a complete, signed DISCOVER frame.
///
/// The signed region is everything before the tag — EN_DISCOVER_SIGNED_LEN,
/// which EspNowProto.h static_asserts against offsetof(DiscoverMsg, tag) so a
/// field added in the wrong place breaks the build rather than the pairing.
static inline bool espnowFillDiscover(DiscoverMsg& d, const uint8_t* key16,
                                      uint8_t nodeId, const uint8_t mac[6],
                                      uint32_t nonce) {
    memset(&d, 0, sizeof(d));
    d.magic  = ESPNOW_MAGIC;
    d.ver    = ESPNOW_PROTO_VER;
    d.type   = EN_MSG_DISCOVER;
    d.nodeId = nodeId;
    memcpy(d.mac, mac, 6);
    d.nonce  = nonce;
    return espnowSignTag(key16, (const uint8_t*)&d, EN_DISCOVER_SIGNED_LEN, d.tag);
}

/// Build a complete, signed WELCOME frame.
///
/// The caller fills in the payload fields — channel, interval, epoch, SSID,
/// BSSID — and this stamps the header, the target and the tag. Same reasoning
/// as espnowFillDiscover(): the side that signs and the side that verifies
/// must agree on the covered region exactly, so neither writes its own.
static inline bool espnowSignWelcome(WelcomeMsg& w, const uint8_t* key16,
                                     uint8_t nodeId, const uint8_t target[6]) {
    w.magic  = ESPNOW_MAGIC;
    w.ver    = ESPNOW_PROTO_VER;
    w.type   = EN_MSG_WELCOME;
    w.nodeId = nodeId;
    memcpy(w.target, target, 6);
    return espnowSignTag(key16, (const uint8_t*)&w, EN_WELCOME_SIGNED_LEN, w.tag);
}
