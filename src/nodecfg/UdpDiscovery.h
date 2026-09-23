// ============================================================================
// src/nodecfg/UdpDiscovery.h
//
// Finding the collector on the LAN when its address stopped answering —
// docs/NODE_CONFIG.md §3.1. Built and checked here; sent and received by the
// firmwares.
//
// THE PACKETS
// -----------
//   query  (node → broadcast, UDP 47810), 37 bytes:
//       "ESPL?" | nonce[8] | name[16, zero padded] | tag[8]
//   reply  (collector → the node, unicast), 23 bytes:
//       "ESPL!" | nonce[8] echoed | http port u16 LE | tag[8]
//
//   tag = first 8 bytes of HMAC-SHA256(key = the ingest token, every byte
//         before the tag)
//
// WHY THE TAG, AND WHAT IT DOES NOT BUY
// -------------------------------------
// A node that believes a reply will POST its readings — and, in the reply to
// those, accept a config WITH SECRETS — to whatever answered. So the answer
// has to prove it comes from something holding the ingest token, and the
// question has to as well, or anyone on the LAN could map every node by name.
// The nonce, echoed and covered by the reply's tag, stops an old captured
// reply being replayed at a node that has moved on.
//
// It is not secrecy: the node's name travels in the clear, as it already does
// in every ingest POST. And with no token configured on either side the key
// is empty — the HMAC is still well defined and both ends still agree, but it
// then proves nothing, which is the same guarantee /api/ingest gives without
// a token.
//
// WHY THE HMAC IS A PARAMETER
// ---------------------------
// The ESP8266 has BearSSL and no mbedTLS; the collector has mbedTLS; the host
// tests have neither. Every function takes the HMAC as a plain function
// pointer, so the byte layout — which is what the two ends must agree on —
// lives in this one header and is tested on the host against a reference
// SHA-256, while each firmware plugs in its own crypto. UdpDiscoveryHmac.h
// has the two firmware adapters.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "NodeConfig.h"

namespace nodecfg {
namespace udpdisc {

/// Full HMAC-SHA256 of `data` under `key` into `out` (32 bytes). Returns
/// false only if the crypto library itself failed; the caller then treats the
/// packet as unverifiable.
typedef bool (*HmacSha256Fn)(const uint8_t* key, size_t keyLen,
                             const uint8_t* data, size_t len, uint8_t out[32]);

static const uint16_t DISCOVERY_PORT = 47810;
static const size_t   MAGIC_LEN = 5;
static const size_t   NONCE_LEN = 8;
static const size_t   NAME_LEN  = 16;   ///< == NODE_NAME_MAX: a full name has no NUL on the wire
static const size_t   TAG_LEN   = 8;
static const size_t   QUERY_LEN = MAGIC_LEN + NONCE_LEN + NAME_LEN + TAG_LEN;   // 37
static const size_t   REPLY_LEN = MAGIC_LEN + NONCE_LEN + 2 + TAG_LEN;          // 23

static const char QUERY_MAGIC[] = "ESPL?";
static const char REPLY_MAGIC[] = "ESPL!";

static_assert(QUERY_LEN == 37, "§3.1: 5 + 8 + 16 + 8");
static_assert(REPLY_LEN == 23, "§3.1: 5 + 8 + 2 + 8");
static_assert(NAME_LEN == NODE_NAME_MAX, "the query carries a whole node name");

/// tag = HMAC(token, buf[0..signedLen))[0..8)
static inline bool computeTag(const char* token, const uint8_t* buf, size_t signedLen,
                              HmacSha256Fn hmac, uint8_t tag[TAG_LEN]) {
    if (!hmac || !buf) return false;
    const char* k = token ? token : "";
    uint8_t full[32];
    if (!hmac((const uint8_t*)k, strlen(k), buf, signedLen, full)) return false;
    memcpy(tag, full, TAG_LEN);
    return true;
}

/// Constant-time tag check, for the same reason as espnowVerifyTag(): free to
/// get right, and a byte-at-a-time memcmp on a value an attacker can iterate
/// is awkward to explain afterwards.
static inline bool tagMatches(const char* token, const uint8_t* buf, size_t signedLen,
                              HmacSha256Fn hmac) {
    uint8_t want[TAG_LEN];
    if (!computeTag(token, buf, signedLen, hmac, want)) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < TAG_LEN; i++) diff |= (uint8_t)(want[i] ^ buf[signedLen + i]);
    return diff == 0;
}

/// Node side: build the broadcast query into `out`. `nonce` must be fresh
/// random bytes per query (os_random / esp_random) — it is what the reply
/// must echo. False if `name` is empty or longer than NAME_LEN, or the HMAC
/// failed.
static inline bool buildQuery(uint8_t out[QUERY_LEN], const uint8_t nonce[NONCE_LEN],
                              const char* name, const char* token, HmacSha256Fn hmac) {
    if (!out || !nonce || !name) return false;
    const size_t nl = strlen(name);
    if (nl == 0 || nl > NAME_LEN) return false;
    memcpy(out, QUERY_MAGIC, MAGIC_LEN);
    memcpy(out + MAGIC_LEN, nonce, NONCE_LEN);
    memset(out + MAGIC_LEN + NONCE_LEN, 0, NAME_LEN);
    memcpy(out + MAGIC_LEN + NONCE_LEN, name, nl);
    const size_t signedLen = QUERY_LEN - TAG_LEN;
    return computeTag(token, out, signedLen, hmac, out + signedLen);
}

/// Collector side: is `buf` a query signed with our token? On success copies
/// out the nonce (to echo) and the node's name (NUL-terminated).
///
/// Refuses anything whose length is not exactly QUERY_LEN, whose magic is
/// wrong, whose name is empty or has bytes after its first NUL (a padded name
/// is zeros to the end — anything else is not a name this code wrote), or
/// whose tag does not verify.
static inline bool parseQuery(const uint8_t* buf, size_t len, const char* token,
                              HmacSha256Fn hmac, uint8_t nonceOut[NONCE_LEN],
                              char nameOut[NAME_LEN + 1]) {
    if (!buf || len != QUERY_LEN) return false;
    if (memcmp(buf, QUERY_MAGIC, MAGIC_LEN) != 0) return false;
    const uint8_t* nm = buf + MAGIC_LEN + NONCE_LEN;
    if (nm[0] == 0) return false;
    bool ended = false;
    for (size_t i = 0; i < NAME_LEN; i++) {
        if (nm[i] == 0) ended = true;
        else if (ended) return false;
    }
    if (!tagMatches(token, buf, QUERY_LEN - TAG_LEN, hmac)) return false;
    if (nonceOut) memcpy(nonceOut, buf + MAGIC_LEN, NONCE_LEN);
    if (nameOut) {
        memcpy(nameOut, nm, NAME_LEN);
        nameOut[NAME_LEN] = '\0';
    }
    return true;
}

/// Collector side: build the reply to a query whose nonce is `nonce`.
/// `httpPort` is the port the collector's web server (and /api/ingest)
/// listens on — little-endian on the wire whatever the host's order.
static inline bool buildReply(uint8_t out[REPLY_LEN], const uint8_t nonce[NONCE_LEN],
                              uint16_t httpPort, const char* token, HmacSha256Fn hmac) {
    if (!out || !nonce) return false;
    memcpy(out, REPLY_MAGIC, MAGIC_LEN);
    memcpy(out + MAGIC_LEN, nonce, NONCE_LEN);
    out[MAGIC_LEN + NONCE_LEN]     = (uint8_t)(httpPort & 0xFF);
    out[MAGIC_LEN + NONCE_LEN + 1] = (uint8_t)(httpPort >> 8);
    const size_t signedLen = REPLY_LEN - TAG_LEN;
    return computeTag(token, out, signedLen, hmac, out + signedLen);
}

/// Node side: is `buf` the collector's answer to OUR query (the nonce we
/// sent), signed with our token? On success writes the HTTP port. The sender's
/// IP address — which the caller has from the socket — becomes net.host.
/// A port of 0 is refused: it cannot be the collector's web server.
static inline bool parseReply(const uint8_t* buf, size_t len,
                              const uint8_t expectNonce[NONCE_LEN], const char* token,
                              HmacSha256Fn hmac, uint16_t& portOut) {
    if (!buf || !expectNonce || len != REPLY_LEN) return false;
    if (memcmp(buf, REPLY_MAGIC, MAGIC_LEN) != 0) return false;
    if (memcmp(buf + MAGIC_LEN, expectNonce, NONCE_LEN) != 0) return false;
    if (!tagMatches(token, buf, REPLY_LEN - TAG_LEN, hmac)) return false;
    const uint16_t port = (uint16_t)(buf[MAGIC_LEN + NONCE_LEN] |
                                     ((uint16_t)buf[MAGIC_LEN + NONCE_LEN + 1] << 8));
    if (port == 0) return false;
    portOut = port;
    return true;
}

}  // namespace udpdisc
}  // namespace nodecfg
