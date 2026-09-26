// The WiFi node's firmware-offer decisions — node/src/FwOffer.h.
//
// WHY THIS FILE EXISTS
// --------------------
// docs/NODE_OTA.md §3 gives the ESP8266 node three rules about the `fw` the
// collector puts in an ingest reply, and each fails without a symptom anyone
// on the bench would see:
//
//   * an offer that failed, tried again every cycle, is half a megabyte of
//     flash erased and rewritten every minute until the chip wears out — and
//     looks, from the collector's side, like a node that is "sending";
//   * a fw_error cleared before a POST carrying it was answered leaves the
//     target `sending` for ever; one cleared by the answer to an OLDER report
//     loses the newer failure the same way;
//   * an offer of the image the node already runs, taken, is a download and
//     a restart for nothing, every cycle until the collector hears fw_md5.
//
// And the verdict on a whole image — header, marker, kind — which is what
// stands between a wrong .bin and a node that no longer boots.
#include "node/src/FwOffer.h"
#include "check.h"

using namespace NodeFw;

static const char* MD5_A = "0123456789abcdef0123456789abcdef";
static const char* MD5_B = "fedcba9876543210fedcba9876543210";
static const uint32_t ROOM = 0xFF000;

static Offer offer(const char* md5, uint32_t attempt, uint32_t size = 466848) {
    Offer o;
    o.present = true;
    nodecfg::copyStr(o.md5, sizeof(o.md5), md5);
    nodecfg::copyStr(o.path, sizeof(o.path), "/api/nodes/fw/bin?kind=esp8266");
    nodecfg::copyStr(o.ver, sizeof(o.ver), "2026.10.1");
    o.size = size;
    o.attempt = attempt;
    return o;
}

// ---------------------------------------------------------------------------
static void test_md5_text() {
    CHECK(isMd5Hex(MD5_A));
    CHECK(isMd5Hex("0123456789ABCDEF0123456789ABCDEF"));
    CHECK(!isMd5Hex(""));
    CHECK(!isMd5Hex(nullptr));
    CHECK(!isMd5Hex("0123456789abcdef0123456789abcde"));     // 31
    CHECK(!isMd5Hex("0123456789abcdef0123456789abcdef0"));   // 33
    CHECK(!isMd5Hex("0123456789abcdef0123456789abcdeg"));
    // Case does not make two images different.
    CHECK(sameMd5(MD5_A, "0123456789ABCDEF0123456789ABCDEF"));
    CHECK(!sameMd5(MD5_A, MD5_B));
    CHECK(!sameMd5(MD5_A, ""));
    CHECK(!sameMd5(MD5_A, nullptr));
}

static void test_what_to_do_with_an_offer() {
    Report r;
    Offer none;
    CHECK(decideOffer(none, MD5_B, r, ROOM) == OfferAction::None);

    CHECK(decideOffer(offer(MD5_A, 1), MD5_B, r, ROOM) == OfferAction::Download);
    // The image it already runs: the next fw_md5 settles it, no download.
    CHECK(decideOffer(offer(MD5_A, 1), MD5_A, r, ROOM) == OfferAction::Running);
    CHECK(decideOffer(offer("0123456789ABCDEF0123456789ABCDEF", 1), MD5_A, r, ROOM) ==
          OfferAction::Running);
    // A node whose own MD5 could not be read still takes an update.
    CHECK(decideOffer(offer(MD5_A, 1), "", r, ROOM) == OfferAction::Download);

    // What it cannot act on is Invalid — and reported, not ignored.
    Offer o = offer("not-an-md5", 1);
    CHECK(decideOffer(o, MD5_B, r, ROOM) == OfferAction::Invalid);
    CHECK_STREQ(invalidReason(o, ROOM), "bad_offer");
    o = offer(MD5_A, 1, 0);
    CHECK(decideOffer(o, MD5_B, r, ROOM) == OfferAction::Invalid);
    o = offer(MD5_A, 1);
    o.path[0] = '\0';
    CHECK(decideOffer(o, MD5_B, r, ROOM) == OfferAction::Invalid);
    nodecfg::copyStr(o.path, sizeof(o.path), "http://elsewhere/x.bin");   // never another host
    CHECK(decideOffer(o, MD5_B, r, ROOM) == OfferAction::Invalid);

    // Larger than the space this node has, or than the kind's slot.
    o = offer(MD5_A, 1, 500000);
    CHECK(decideOffer(o, MD5_B, r, 400000) == OfferAction::Invalid);
    CHECK_STREQ(invalidReason(o, 400000), "too_big");
    o = offer(MD5_A, 1, 0xFF001);
    CHECK(decideOffer(o, MD5_B, r, 0x200000) == OfferAction::Invalid);
    CHECK_STREQ(invalidReason(o, 0x200000), "too_big");
    o = offer(MD5_A, 1, 0xFF000);
    CHECK(decideOffer(o, MD5_B, r, 0x200000) == OfferAction::Download);
}

static void test_a_failed_offer_is_not_retried_until_the_collector_asks_again() {
    Report r;
    r.fail(MD5_A, 3, "md5_mismatch");
    // §3 step 1: the same (md5, attempt) is not tried again this boot...
    CHECK(decideOffer(offer(MD5_A, 3), MD5_B, r, ROOM) == OfferAction::AlreadyFailed);
    CHECK(decideOffer(offer("0123456789ABCDEF0123456789ABCDEF", 3), MD5_B, r, ROOM) ==
          OfferAction::AlreadyFailed);
    // ...but a Retry on the collector bumps attempt (§4.5), and a new image
    // is a new image.
    CHECK(decideOffer(offer(MD5_A, 4), MD5_B, r, ROOM) == OfferAction::Download);
    CHECK(decideOffer(offer(MD5_B, 3), MD5_A, r, ROOM) == OfferAction::Download);
    // Only the latest failure is remembered: one image per kind on the
    // collector, so there is never an older offer to come back to.
    r.fail(MD5_A, 4, "timeout");
    CHECK(decideOffer(offer(MD5_A, 4), MD5_B, r, ROOM) == OfferAction::AlreadyFailed);
    // A node that already runs it is told so even after a failed attempt.
    CHECK(decideOffer(offer(MD5_A, 4), MD5_A, r, ROOM) == OfferAction::Running);
}

static void test_fw_error_is_held_until_a_post_carrying_it_is_answered() {
    Report r;
    CHECK(!r.pending());
    CHECK_EQ(r.token(), 0);
    r.delivered(0);                 // a POST that carried nothing
    CHECK(!r.pending());

    r.fail(MD5_A, 3, "not_node_image");
    CHECK(r.pending());
    const uint16_t t1 = r.token();
    CHECK(t1 != 0);
    CHECK_STREQ(r.md5(), MD5_A);
    CHECK_EQ(r.attempt(), 3u);
    CHECK_STREQ(r.reason(), "not_node_image");

    // The POST that carried it failed: nothing is delivered, nothing cleared.
    CHECK(r.pending());
    CHECK_EQ(r.token(), t1);

    // A second failure is recorded between building a POST that carries the
    // first and hearing its answer.
    r.fail(MD5_A, 4, "timeout");
    const uint16_t t2 = r.token();
    CHECK(t2 != t1);
    r.delivered(t1);                // the answer to the OLD report
    CHECK(r.pending());             // the new one is still to be told
    CHECK_STREQ(r.reason(), "timeout");
    r.delivered(t2);
    CHECK(!r.pending());
    CHECK_EQ(r.token(), 0);
    // Delivered is not forgotten: still not retried this boot.
    CHECK(r.failed(MD5_A, 4));

    // A reason longer than the field is cut, never overrun.
    r.fail(MD5_B, 1, "a reason far longer than forty characters, to be cut short");
    CHECK(strlen(r.reason()) == REASON_CAP - 1);
}

static void test_the_token_never_reads_as_nothing() {
    Report r;
    for (int i = 0; i < 70000; i++) {
        r.fail(MD5_A, (uint32_t)i, "x");
        if (r.token() == 0) { CHECK(false); break; }
    }
    CHECK(r.pending());
}

static void feed(nodefw::MarkerScan& s, const char* text) {
    nodefw::scanFeed(s, (const uint8_t*)text, strlen(text) + 1);   // with its NUL
}

static void test_the_verdict_on_a_whole_image() {
    nodefw::MarkerScan s;
    nodefw::scanBegin(s);
    feed(s, "NODEFW1|esp8266|2026.10.1|");
    CHECK(imageIsOurs(true, true, s));
    // A header that is not an ESP image, or none at all (an empty upload).
    CHECK(!imageIsOurs(true, false, s));
    CHECK(!imageIsOurs(false, false, s));

    nodefw::scanBegin(s);
    CHECK(!imageIsOurs(true, true, s));                 // no marker: some other sketch
    feed(s, "NODEFW1|espnow-c3|2026.10.1|");
    CHECK(!imageIsOurs(true, true, s));                 // the ESP-NOW node's image

    nodefw::scanBegin(s);
    feed(s, "NODEFW1|esp8266|2026.10.1|");
    feed(s, "NODEFW1|espnow-c3|2026.10.1|");
    CHECK(!imageIsOurs(true, true, s));                 // two that disagree
}

int main() {
    RUN(test_md5_text);
    RUN(test_what_to_do_with_an_offer);
    RUN(test_a_failed_offer_is_not_retried_until_the_collector_asks_again);
    RUN(test_fw_error_is_held_until_a_post_carrying_it_is_answered);
    RUN(test_the_token_never_reads_as_nothing);
    RUN(test_the_verdict_on_a_whole_image);
    return SUMMARY();
}
