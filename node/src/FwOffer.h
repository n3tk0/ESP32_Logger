// ============================================================================
// node/src/FwOffer.h — the WiFi node's decisions about a firmware offer, with
// nothing in them that needs a board.
//
// docs/NODE_OTA.md §3. The collector's ingest reply may carry
//
//     "fw": {"path","md5","size","ver","attempt"}
//
// and the node, after the cycle's POSTs, downloads that image into its update
// slot and restarts into it — or fails, and says so as `fw_error` until the
// collector has heard. main.cpp does the I/O (HTTPClient, Update); this header
// says whether an offer is worth acting on and keeps the fw_error bookkeeping,
// for the reason NodeSync.h exists: tests/host reaches it, and each rule here
// is one of the quiet kinds of wrong.
//
//   * An offer retried after it failed is a node that downloads half a
//     megabyte every cycle — at a 60 s interval, the collector's whole
//     evening — and fails the same way each time. §3 step 1: the exact
//     (md5, attempt) that failed is not tried again this boot. A Retry on the
//     collector bumps `attempt`, so a person asking again always gets a try.
//   * A failure that is never reported leaves the target `sending` on the
//     collector for ever; one cleared before a POST carrying it was ANSWERED
//     is the same thing, one bad cycle later. Held until delivered, exactly
//     like cfg_error (NodeSync::CfgError).
//   * A second failure that lands while the first is still in flight must not
//     be cleared by the first one's answer: the token below says WHICH report
//     a POST carried.
//
// Nothing here allocates, prints or touches hardware.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "src/nodecfg/FwImage.h"
#include "src/nodecfg/NodeConfig.h"   // nodecfg::copyStr

namespace NodeFw {

static const size_t MD5_HEX  = 32;
static const size_t MD5_CAP  = MD5_HEX + 1;
/// "/api/nodes/fw/bin?kind=esp8266" is 30; room for a collector that grows a
/// query parameter without the node truncating it into a 404.
static const size_t PATH_CAP = 80;
/// Fits every reason main.cpp and FwFlash produce, and the collector's
/// column for it (NODE_OTA.md §2.2 `err`).
static const size_t REASON_CAP = 40;

/// The reply's `fw`, as far as the decision needs it. main.cpp fills it from
/// the JSON; nothing is checked there — decideOffer() does the checking.
struct Offer {
    bool     present = false;
    char     md5[MD5_CAP] = "";
    char     path[PATH_CAP] = "";
    char     ver[nodefw::VER_CAP] = "";
    uint32_t size = 0;
    uint32_t attempt = 0;
};

static inline bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Exactly 32 hex digits. Update.setMD5() refuses any other length silently
/// (it returns false and the download would run with no MD5 check at all),
/// so a malformed one is refused here, before a byte is erased.
static inline bool isMd5Hex(const char* s) {
    if (!s) return false;
    for (size_t i = 0; i < MD5_HEX; i++)
        if (!isHex(s[i])) return false;
    return s[MD5_HEX] == '\0';
}

/// MD5 text compared without regard to case: the contract says lowercase,
/// and ESP.getSketchMD5() gives lowercase, but a collector that upper-cases
/// it would otherwise have every node download the image it already runs.
static inline bool sameMd5(const char* a, const char* b) {
    if (!a || !b) return false;
    for (size_t i = 0; i <= MD5_HEX; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (x == '\0') return true;
    }
    return true;
}

enum class OfferAction : uint8_t {
    None,           ///< no offer in the reply
    Download,       ///< fetch it, check it, restart into it
    Running,        ///< the image IS this firmware — the next fw_md5 says so
    AlreadyFailed,  ///< this (md5, attempt) failed since boot (§3 step 1)
    Invalid,        ///< an offer this node cannot act on (reported as fw_error)
};

/// The (md5, attempt) that failed since boot and the report still to deliver.
class Report {
public:
    /// Remember a failure (§3 step 5): not tried again, and reported as
    /// fw_error with every POST until one carrying it is answered.
    void fail(const char* md5, uint32_t attempt, const char* reason) {
        nodecfg::copyStr(_md5, sizeof(_md5), md5 ? md5 : "");
        nodecfg::copyStr(_reason, sizeof(_reason), reason ? reason : "");
        _attempt = attempt;
        _failed  = true;
        // Never 0: 0 is what token() says when there is nothing to send.
        if (++_seq == 0) _seq = 1;
        _pending = _seq;
    }

    bool failed(const char* md5, uint32_t attempt) const {
        return _failed && attempt == _attempt && sameMd5(md5, _md5);
    }

    /// Which report a POST carries: 0 = none. Take it when the body is built,
    /// hand it to delivered() once that POST has been answered.
    uint16_t token() const { return _pending; }
    bool pending() const { return _pending != 0; }
    /// A POST carrying report `tok` was answered. A newer failure recorded
    /// in the meantime has a different token and stays pending.
    void delivered(uint16_t tok) {
        if (tok != 0 && tok == _pending) _pending = 0;
    }

    const char* md5() const { return _md5; }
    uint32_t attempt() const { return _attempt; }
    const char* reason() const { return _reason; }

private:
    char     _md5[MD5_CAP] = "";
    char     _reason[REASON_CAP] = "";
    uint32_t _attempt = 0;
    bool     _failed  = false;
    uint16_t _seq     = 0;
    uint16_t _pending = 0;
};

/// What to do with the reply's `fw`, given the MD5 of the sketch this node
/// runs and what has failed since boot.
///
/// `maxBytes` is the largest image this node can take: the free sketch space
/// on the device (it shrinks as this firmware grows), nodefw::maxSize() in a
/// test. An offer bigger than that would erase a slot's worth of flash before
/// Update noticed; refused here instead, as Invalid, so it is reported.
static inline OfferAction decideOffer(const Offer& o, const char* runningMd5,
                                      const Report& rep, uint32_t maxBytes) {
    if (!o.present) return OfferAction::None;
    if (!isMd5Hex(o.md5) || o.size == 0 || o.path[0] != '/') return OfferAction::Invalid;
    // Before the size check: a node can always be told it already runs this.
    if (sameMd5(o.md5, runningMd5)) return OfferAction::Running;
    if (rep.failed(o.md5, o.attempt)) return OfferAction::AlreadyFailed;
    if (o.size > maxBytes || o.size > nodefw::maxSize(nodefw::KIND_ESP8266))
        return OfferAction::Invalid;
    return OfferAction::Download;
}

/// Why decideOffer() said Invalid, as the fw_error reason.
static inline const char* invalidReason(const Offer& o, uint32_t maxBytes) {
    if (!isMd5Hex(o.md5) || o.path[0] != '/') return "bad_offer";
    if (o.size == 0) return "bad_offer";
    if (o.size > maxBytes || o.size > nodefw::maxSize(nodefw::KIND_ESP8266)) return "too_big";
    return "bad_offer";
}

/// The verdict on a whole image, once every byte has passed the scanner:
/// §1.1 — no marker, two that disagree, or another kind is not ours.
static inline bool imageIsOurs(bool headSeen, bool headOk, const nodefw::MarkerScan& s) {
    return headSeen && headOk && nodefw::scanKind(s) == nodefw::KIND_ESP8266;
}

}  // namespace NodeFw
