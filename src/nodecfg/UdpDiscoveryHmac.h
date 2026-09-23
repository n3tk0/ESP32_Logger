// ============================================================================
// src/nodecfg/UdpDiscoveryHmac.h
//
// The HMAC-SHA256 each firmware plugs into UdpDiscovery.h.
//
// Kept out of UdpDiscovery.h for the reason EspNowAuth.h is kept out of
// EspNowProto.h: the packet layout is what both ends must agree on and what
// the host tests check, and it must compile with no crypto library at all.
// This file is the opposite — nothing but crypto glue — and is included only
// by firmware.
//
//   ESP8266 (node/)     BearSSL, which the core ships for its TLS client.
//                       The core has no mbedTLS.
//   ESP32   (src/)      mbedTLS, which ESP-IDF ships and EspNowAuth.h
//                       already uses.
//
// Pass udpdiscHmacSha256 to the udpdisc:: functions on either.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP8266)
#  include <bearssl/bearssl_hmac.h>
#elif defined(ESP_PLATFORM)
#  include <mbedtls/md.h>
#else
#  error "UdpDiscoveryHmac.h is firmware-only; host tests supply their own HMAC"
#endif

namespace nodecfg {
namespace udpdisc {

static inline bool udpdiscHmacSha256(const uint8_t* key, size_t keyLen,
                                     const uint8_t* data, size_t len, uint8_t out[32]) {
    if (!out || (!data && len)) return false;
#if defined(ESP8266)
    br_hmac_key_context kc;
    br_hmac_context ctx;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, keyLen);
    br_hmac_init(&ctx, &kc, 0);
    br_hmac_update(&ctx, data, len);
    return br_hmac_out(&ctx, out) == 32;
#else
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    // mbedTLS wants a non-null pointer even for an empty key; the empty key
    // is legitimate (a collector with no INGEST_TOKEN), so give it one.
    static const uint8_t none = 0;
    return mbedtls_md_hmac(info, keyLen ? key : &none, keyLen, data, len, out) == 0;
#endif
}

}  // namespace udpdisc
}  // namespace nodecfg
