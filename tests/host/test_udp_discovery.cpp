// Host unit tests for src/nodecfg/UdpDiscovery.h — how a WiFi node finds the
// collector again after its address stopped answering (docs/NODE_CONFIG.md
// §3.1).
//
// The node believes whatever answers this: it will POST its readings there
// and take a config WITH SECRETS from the reply. So the properties worth
// pinning are all about refusing — a wrong token, a tampered byte, an old
// reply to a different question, a packet one byte long or short — plus the
// byte layout itself, which the ESP8266 (BearSSL) and the collector (mbedTLS)
// must agree on without ever having been tested against each other.
//
// Neither crypto library exists here, so this file carries a small reference
// SHA-256 / HMAC-SHA256 and first proves IT against RFC 4231 — a test that
// passed against a broken HMAC would prove nothing about the real ones.
#include <stdint.h>
#include <string.h>

#include "src/nodecfg/UdpDiscovery.h"
#include "check.h"

using namespace nodecfg::udpdisc;

// ---------------------------------------------------------------------------
// Reference SHA-256 (FIPS 180-4) and HMAC (RFC 2104). Straightforward and
// slow; correctness is checked against the RFC 4231 vectors below.
// ---------------------------------------------------------------------------
namespace ref {

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t h[8];
    uint8_t  blk[64];
    size_t   used;
    uint64_t bits;

    void init() {
        static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        memcpy(h, H0, sizeof(h));
        used = 0;
        bits = 0;
    }
    void compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 |
                   (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        bits += (uint64_t)n * 8;
        while (n--) {
            blk[used++] = *p++;
            if (used == 64) { compress(); used = 0; }
        }
    }
    void final(uint8_t out[32]) {
        const uint64_t total = bits;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(total >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; i++) {
            out[4 * i]     = (uint8_t)(h[i] >> 24);
            out[4 * i + 1] = (uint8_t)(h[i] >> 16);
            out[4 * i + 2] = (uint8_t)(h[i] >> 8);
            out[4 * i + 3] = (uint8_t)h[i];
        }
    }
};

static bool hmac(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len,
                 uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) {
        Sha256 s; s.init(); s.update(key, keyLen); s.final(k);
    } else if (keyLen) {
        memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 s;
    s.init(); s.update(ipad, 64); s.update(data, len); s.final(inner);
    s.init(); s.update(opad, 64); s.update(inner, 32); s.final(out);
    return true;
}

}  // namespace ref

// An HMAC that always fails, as a crypto library can.
static bool brokenHmac(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) {
    return false;
}

static void hexTo(const char* hex, uint8_t* out) {
    for (size_t i = 0; hex[2 * i]; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            const char c = hex[2 * i + j];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
}

// ---------------------------------------------------------------------------
static void test_reference_hmac_matches_rfc4231() {
    uint8_t out[32], want[32];

    // Test case 1
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    ref::hmac(key1, 20, (const uint8_t*)"Hi There", 8, out);
    hexTo("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", want);
    CHECK(memcmp(out, want, 32) == 0);

    // Test case 2: a short key
    ref::hmac((const uint8_t*)"Jefe", 4,
              (const uint8_t*)"what do ya want for nothing?", 28, out);
    hexTo("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    CHECK(memcmp(out, want, 32) == 0);

    // Test case 6: a key longer than the block, hashed first
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    const char* msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    ref::hmac(key6, sizeof(key6), (const uint8_t*)msg6, strlen(msg6), out);
    hexTo("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", want);
    CHECK(memcmp(out, want, 32) == 0);
}

// ---------------------------------------------------------------------------
static const uint8_t NONCE[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t IP[4]    = { 192, 168, 1, 50 };   // the collector, first octet first
static const char*   TOKEN    = "s3cret-ingest-token";

static void test_query_layout() {
    uint8_t q[QUERY_LEN];
    CHECK(buildQuery(q, NONCE, "balcony", TOKEN, ref::hmac));

    CHECK(memcmp(q, "ESPL?", 5) == 0);
    CHECK(memcmp(q + 5, NONCE, 8) == 0);
    CHECK(memcmp(q + 13, "balcony", 7) == 0);
    for (int i = 13 + 7; i < 13 + 16; i++) CHECK_EQ(q[i], 0);   // zero padded

    // The tag is the first 8 bytes of HMAC(token, the 29 bytes before it) —
    // computed independently here, so a change to the covered region fails.
    uint8_t full[32];
    ref::hmac((const uint8_t*)TOKEN, strlen(TOKEN), q, 29, full);
    CHECK(memcmp(q + 29, full, 8) == 0);
}

static void test_query_round_trip_and_refusals() {
    uint8_t q[QUERY_LEN];
    CHECK(buildQuery(q, NONCE, "balcony", TOKEN, ref::hmac));

    uint8_t nonce[8];
    char name[17];
    CHECK(parseQuery(q, sizeof(q), TOKEN, ref::hmac, nonce, name));
    CHECK(memcmp(nonce, NONCE, 8) == 0);
    CHECK_STREQ(name, "balcony");

    // Wrong token: a stranger's node, or a collector for another house.
    CHECK(!parseQuery(q, sizeof(q), "other-token", ref::hmac, nonce, name));
    // Every single byte is covered: flip each one and the packet is refused.
    for (size_t i = 0; i < QUERY_LEN; i++) {
        uint8_t t[QUERY_LEN];
        memcpy(t, q, sizeof(t));
        t[i] ^= 0x01;
        CHECK(!parseQuery(t, sizeof(t), TOKEN, ref::hmac, nonce, name));
    }
    // One byte short, one byte long.
    CHECK(!parseQuery(q, QUERY_LEN - 1, TOKEN, ref::hmac, nonce, name));
    {
        uint8_t l[QUERY_LEN + 1];
        memcpy(l, q, QUERY_LEN);
        l[QUERY_LEN] = 0;
        CHECK(!parseQuery(l, sizeof(l), TOKEN, ref::hmac, nonce, name));
    }
    // A reply is not a query.
    uint8_t r[REPLY_LEN];
    CHECK(buildReply(r, NONCE, 80, IP, TOKEN, ref::hmac));
    CHECK(!parseQuery(r, sizeof(r), TOKEN, ref::hmac, nonce, name));
    // A crypto failure is a refusal, never a pass.
    CHECK(!parseQuery(q, sizeof(q), TOKEN, brokenHmac, nonce, name));
    CHECK(!buildQuery(q, NONCE, "balcony", TOKEN, brokenHmac));
    CHECK(!parseQuery(nullptr, QUERY_LEN, TOKEN, ref::hmac, nonce, name));
}

static void test_query_names() {
    uint8_t q[QUERY_LEN];
    uint8_t nonce[8];
    char name[17];

    // A 16-character name fills the field with no terminator on the wire and
    // comes back terminated.
    CHECK(buildQuery(q, NONCE, "abcdefghijklmnop", TOKEN, ref::hmac));
    CHECK(parseQuery(q, sizeof(q), TOKEN, ref::hmac, nonce, name));
    CHECK_STREQ(name, "abcdefghijklmnop");

    CHECK(!buildQuery(q, NONCE, "abcdefghijklmnopq", TOKEN, ref::hmac));   // 17
    CHECK(!buildQuery(q, NONCE, "", TOKEN, ref::hmac));
    CHECK(!buildQuery(q, NONCE, nullptr, TOKEN, ref::hmac));

    // Bytes after the name's NUL: not something buildQuery() writes, so not a
    // name — even when correctly signed by someone holding the token.
    CHECK(buildQuery(q, NONCE, "ab", TOKEN, ref::hmac));
    q[13 + 5] = 'x';
    uint8_t full[32];
    ref::hmac((const uint8_t*)TOKEN, strlen(TOKEN), q, 29, full);
    memcpy(q + 29, full, 8);
    CHECK(!parseQuery(q, sizeof(q), TOKEN, ref::hmac, nonce, name));
    // And an empty name, signed, likewise.
    memset(q + 13, 0, 16);
    ref::hmac((const uint8_t*)TOKEN, strlen(TOKEN), q, 29, full);
    memcpy(q + 29, full, 8);
    CHECK(!parseQuery(q, sizeof(q), TOKEN, ref::hmac, nonce, name));
}

// ---------------------------------------------------------------------------
static void test_reply_layout_and_round_trip() {
    uint8_t r[REPLY_LEN];
    CHECK(buildReply(r, NONCE, 0x1F90, IP, TOKEN, ref::hmac));   // 8080
    CHECK(memcmp(r, "ESPL!", 5) == 0);
    CHECK(memcmp(r + 5, NONCE, 8) == 0);
    CHECK_EQ(r[13], 0x90);   // little-endian, whatever the host
    CHECK_EQ(r[14], 0x1F);
    CHECK(memcmp(r + 15, IP, 4) == 0);   // first octet first
    uint8_t full[32];
    ref::hmac((const uint8_t*)TOKEN, strlen(TOKEN), r, 19, full);
    CHECK(memcmp(r + 19, full, 8) == 0);

    uint16_t port = 0;
    CHECK(parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, IP, port));
    CHECK_EQ(port, 8080);
}

static void test_reply_refusals() {
    uint8_t r[REPLY_LEN];
    CHECK(buildReply(r, NONCE, 80, IP, TOKEN, ref::hmac));
    uint16_t port = 1234;

    // An answer to somebody else's question — or to an earlier one of ours:
    // the nonce is what makes a captured reply useless later.
    const uint8_t other[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };
    CHECK(!parseReply(r, sizeof(r), other, TOKEN, ref::hmac, IP, port));
    CHECK(!parseReply(r, sizeof(r), NONCE, "other-token", ref::hmac, IP, port));
    for (size_t i = 0; i < REPLY_LEN; i++) {
        uint8_t t[REPLY_LEN];
        memcpy(t, r, sizeof(t));
        t[i] ^= 0x80;
        CHECK(!parseReply(t, sizeof(t), NONCE, TOKEN, ref::hmac, IP, port));
    }
    CHECK(!parseReply(r, REPLY_LEN - 1, NONCE, TOKEN, ref::hmac, IP, port));
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, brokenHmac, IP, port));
    // Port 0 cannot be a web server, however well signed.
    CHECK(buildReply(r, NONCE, 0, IP, TOKEN, ref::hmac));
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, IP, port));
    CHECK_EQ(port, 1234);   // untouched by every refusal
    // A query is not a reply.
    uint8_t q[QUERY_LEN];
    CHECK(buildQuery(q, NONCE, "balcony", TOKEN, ref::hmac));
    CHECK(!parseReply(q, REPLY_LEN, NONCE, TOKEN, ref::hmac, IP, port));
    CHECK_EQ(port, 1234);
}

static void test_reply_bound_to_sender() {
    // A valid, fresh reply re-sent by another host on the LAN: signed for
    // the collector's address, arriving from someone else's. Refused — the
    // node would otherwise POST its token and take a config from that host.
    uint8_t r[REPLY_LEN];
    CHECK(buildReply(r, NONCE, 80, IP, TOKEN, ref::hmac));
    uint16_t port = 1234;
    const uint8_t other[4] = { 192, 168, 1, 66 };
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, other, port));
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, nullptr, port));
    CHECK_EQ(port, 1234);
    // Rewriting the address to the sender's breaks the tag.
    memcpy(r + 15, other, 4);
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, other, port));
    CHECK(!buildReply(r, NONCE, 80, nullptr, TOKEN, ref::hmac));
    // The 23-byte reply of earlier firmware is refused by length, and a new
    // reply by length at an old node: neither side mistakes the other's.
    CHECK(!parseReply(r, 23, NONCE, TOKEN, ref::hmac, IP, port));
    CHECK_EQ(REPLY_LEN, 27u);
}

static void test_empty_token_is_symmetric() {
    // A collector with no INGEST_TOKEN, and a node with none: the HMAC is
    // still defined (empty key) and both ends must agree on it.
    uint8_t q[QUERY_LEN], r[REPLY_LEN];
    uint8_t nonce[8];
    char name[17];
    uint16_t port = 0;
    CHECK(buildQuery(q, NONCE, "attic", "", ref::hmac));
    CHECK(parseQuery(q, sizeof(q), nullptr, ref::hmac, nonce, name));   // null == ""
    CHECK(buildReply(r, nonce, 80, IP, nullptr, ref::hmac));
    CHECK(parseReply(r, sizeof(r), NONCE, "", ref::hmac, IP, port));
    CHECK_EQ(port, 80);
    // …and a node WITH a token refuses a collector without one.
    CHECK(!parseReply(r, sizeof(r), NONCE, TOKEN, ref::hmac, IP, port));
}

int main() {
    RUN(test_reference_hmac_matches_rfc4231);
    RUN(test_query_layout);
    RUN(test_query_round_trip_and_refusals);
    RUN(test_query_names);
    RUN(test_reply_layout_and_round_trip);
    RUN(test_reply_refusals);
    RUN(test_reply_bound_to_sender);
    RUN(test_empty_token_is_symmetric);
    return SUMMARY();
}
