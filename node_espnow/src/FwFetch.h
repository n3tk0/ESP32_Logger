// ============================================================================
// node_espnow/src/FwFetch.h
//
// Downloading a new firmware from the collector — docs/NODE_OTA.md §4.3 — as
// a state machine with no radio and no flash in it, so tests/host can drive
// it against a simulated collector (tests/host/test_espnow_node_fw.cpp). The
// flash side, the verification and the switch are in FwFetch.cpp.
//
// THE SHAPE OF IT
// ---------------
// Same pull as the config (CfgFetch.h): an ACK carrying EN_ACK_FW_PENDING
// makes the node stay awake and ask, one request and one reply at a time:
//
//     FW_GET(ANY, 0) → FW(id, size, attempt, minMv, slice 0)
//                    → [RUNNING | ROLLED_BACK | LOW_BATTERY → FW_DONE, stop]
//     FW_GET(id, n)  → FW(slice n) → write → FW_GET(id, n + len) → …
//
// Every wake opens with FW_GET(ANY, 0), even one that resumes: it costs one
// slice, and it is what lets the collector's answer — a new upload, a
// cancelled rollout, a battery floor raised since the last wake — decide
// before this wake spends anything on a download that no longer exists.
//
// NOT ONE WAKE: ~1 MB IS ~6500 SLICES
// -----------------------------------
// At a few milliseconds a slice plus a 4 KB erase every twenty slices, a
// whole image is tens of seconds of radio in receive (~85 mA). So a wake
// spends at most `budgetMs` (NODE_FW_BUDGET_MS) and stops; how far it got
// lives in RTC memory (Progress) and the next wake carries on from there.
// A wake that gets no reply `maxMisses` times in a row also stops — the
// collector drops requests while it refills its window from the card, so a
// miss or two is ordinary, and a run of them is a collector that is gone.
//
// A power cut loses the RTC copy and the next attempt starts at 0. That is
// the right trade: the partition was being written without anything that
// said which bytes were good, and 1 MB is minutes, not hours.
//
// WHAT IS NOT DOWNLOADED AGAIN
// ----------------------------
// `failed` is the (image, attempt) this node refused or could not write
// (BAD_IMAGE, FLASH_ERROR); `rbImg`/`rbAtt` the one it rolled back from
// (FwTrial.h). Offered either again, the node repeats its FW_DONE on the
// first slice instead of paying for 1 MB to reach the same verdict: the
// collector marks the target failed and stops flagging, and when that
// FW_DONE is lost it asks again and gets the same answer. A new `attempt`
// (the user's Retry, docs/NODE_OTA.md §4.5) is a fresh try.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include "src/espnow/EspNowProto.h"

namespace enfw {

/// Flash erase unit. esp_partition_erase_range() takes whole sectors.
static const uint32_t SECTOR = 4096;

/// How far a download got. Lives in RTC memory across wakes (main.cpp's
/// coldBoot rules): gone on a power cut, a panic, or a new image.
struct Progress {
    uint32_t imgId;     ///< EN_FW_ANY = nothing in progress
    uint32_t size;
    uint32_t written;   ///< bytes at the start of the partition that are this image's
    uint32_t partAddr;  ///< the partition being written; a different one = start over
    uint8_t  attempt;
};

/// An (image, attempt) this node already answered with a failure, and which.
struct Failed {
    uint32_t imgId;
    uint8_t  attempt;
    uint8_t  status;    ///< EspNowFwStatus, 0 = none
};

static inline void forget(Progress& p) { memset(&p, 0, sizeof(p)); }

/// The sectors a write of `len` bytes at `off` ENTERS — every sector whose
/// first byte lies in [off, off + len) — which are the ones to erase before
/// writing. A write that starts mid-sector does not erase that sector: it was
/// entered, and erased, by the write that reached its first byte, possibly
/// on an earlier wake. Returns the first such sector's offset and the count.
static inline uint32_t sectorsEntered(uint32_t off, uint32_t len, uint32_t& first) {
    first = (off + SECTOR - 1) & ~(SECTOR - 1);
    if (!len || first >= off + len) return 0;
    return (off + len - 1 - first) / SECTOR + 1;
}

enum class Step : uint8_t {
    Request,   ///< ask (again) with wantRequest()
    Write,     ///< write `slice` at `slice->offset`, then call wrote()
    Complete,  ///< every byte is in the partition: verify and switch
    Report,    ///< send FW_DONE(`status`, `value`) for (`imgId`, `attempt`) and stop
    Busy,      ///< the collector is sending another node: a later wake
    Nothing,   ///< "nothing for you": forget any partial download and stop
};

struct Fetch {
    uint8_t  nodeId;
    uint32_t runningId;   ///< this firmware's image id (docs/NODE_OTA.md §1.3)
    uint32_t rbImg;       ///< rolled back from (FwTrial.h), 0 = none
    uint8_t  rbAtt;
    uint16_t vbatMv;      ///< 0 = not measured: no battery floor applies
    uint32_t slotSize;    ///< the update partition's size
    uint32_t partAddr;    ///< … and its address
    uint32_t want;        ///< the id to request: EN_FW_ANY until the first answer
    uint32_t offset;      ///< next offset to ask for
    uint8_t  misses;      ///< consecutive requests with no usable reply
    uint8_t  maxMisses;
    uint32_t startMs;
    uint32_t budgetMs;
    uint32_t gained;      ///< bytes written this wake
    // The outcome, for Step::Report and the caller's FW_DONE.
    uint32_t imgId;
    uint8_t  attempt;
    uint8_t  status;
    uint16_t value;
    const FwChunkMsg* slice;   ///< for Step::Write: the frame whose data to write
};

static inline void begin(Fetch& f, uint8_t nodeId, uint32_t runningId, uint32_t rbImg,
                         uint8_t rbAtt, uint16_t vbatMv, uint32_t partAddr, uint32_t slotSize,
                         uint32_t nowMs, uint32_t budgetMs, uint8_t maxMisses) {
    memset(&f, 0, sizeof(f));
    f.nodeId    = nodeId;
    f.runningId = runningId;
    f.rbImg     = rbImg;
    f.rbAtt     = rbAtt;
    f.vbatMv    = vbatMv;
    f.partAddr  = partAddr;
    f.slotSize  = slotSize;
    f.want      = EN_FW_ANY;
    f.maxMisses = maxMisses ? maxMisses : 1;
    f.startMs   = nowMs;
    f.budgetMs  = budgetMs;
}

/// Milliseconds left of the budget (0 = spent). Unsigned difference, so it
/// stays right across the millis() wrap.
static inline uint32_t remainingMs(const Fetch& f, uint32_t nowMs) {
    const uint32_t spent = nowMs - f.startMs;
    return spent >= f.budgetMs ? 0 : f.budgetMs - spent;
}

/// The next request, or false when this wake's attempt is over (budget
/// spent, or `maxMisses` unanswered in a row).
static inline bool wantRequest(const Fetch& f, uint32_t nowMs, FwGetMsg& out) {
    if (f.misses >= f.maxMisses) return false;
    if (remainingMs(f, nowMs) == 0) return false;
    espnowFillFwGet(out, f.nodeId, f.want, f.offset);
    return true;
}

static inline Step report(Fetch& f, uint8_t status, uint16_t value) {
    f.status = status;
    f.value  = value;
    return Step::Report;
}

static inline Step miss(Fetch& f) {
    f.misses++;
    return Step::Request;
}

/// Feed what came back for the last request: a FW frame, or nullptr when the
/// window closed empty. `p` is the RTC progress, updated in place.
static inline Step onReply(Fetch& f, Progress& p, const Failed& failed, const FwChunkMsg* m) {
    if (!m || m->type != EN_MSG_FW || m->nodeId != f.nodeId) return miss(f);
    if (m->flags & EN_FW_BUSY) return Step::Busy;
    if (m->size == 0) {
        // Cancelled, or the image deleted: the bytes in the partition are
        // for nothing now, and a later offer must not resume into them.
        forget(p);
        return Step::Nothing;
    }

    if (f.want == EN_FW_ANY) {
        // The first answer. Decide on it before paying for the rest.
        if (m->offset != 0) return miss(f);
        f.imgId   = m->imgId;
        f.attempt = m->attempt;
        if (m->imgId == f.runningId) return report(f, EN_FW_ST_RUNNING, 0);
        if (failed.status && failed.imgId == m->imgId && failed.attempt == m->attempt)
            return report(f, failed.status, 0);
        if (f.rbImg && m->imgId == f.rbImg && m->attempt == f.rbAtt)
            return report(f, EN_FW_ST_ROLLED_BACK, 0);
        // Only a MEASURED battery defers. A node with no divider reads 0,
        // and "unknown" must not become "flat" and hold it back for ever.
        if (f.vbatMv && m->minMv && f.vbatMv < m->minMv)
            return report(f, EN_FW_ST_LOW_BATTERY, f.vbatMv);
        // The collector refuses both at upload; a node checks what it would
        // write into, because a wrong size here is an erase past the slot.
        if (m->imgId == EN_FW_ANY || m->size > f.slotSize) return report(f, EN_FW_ST_BAD_IMAGE, 0);

        f.want   = m->imgId;
        f.misses = 0;
        if (p.imgId == m->imgId && p.attempt == m->attempt && p.size == m->size &&
            p.partAddr == f.partAddr && p.written <= p.size && p.written > 0) {
            // Resume. The offset-0 slice that came with the answer is already
            // in the partition.
            f.offset = p.written;
            return p.written == p.size ? Step::Complete : Step::Request;
        }
        // A different image, a new attempt, or nothing kept: from byte 0.
        p.imgId    = m->imgId;
        p.size     = m->size;
        p.written  = 0;
        p.partAddr = f.partAddr;
        p.attempt  = m->attempt;
        f.offset   = 0;
    } else if (m->imgId != p.imgId || m->attempt != p.attempt || m->size != p.size) {
        // The image or the attempt changed under us (a new upload, a Retry):
        // ask again what there is, and let the first-answer rules decide.
        f.want   = EN_FW_ANY;
        f.offset = 0;
        return miss(f);
    }

    // A slice. Only the next one is any use: the partition is written in
    // order, and `written` must never count a hole. Its length is whatever
    // it says — a slice ends where the collector's RAM window ends, so one
    // mid-image can be shorter than 200 — and the next request is at
    // offset + len. An empty one (not BUSY, not size 0) is a miss.
    if (m->offset != p.written || m->len == 0) return miss(f);
    f.slice  = m;
    f.misses = 0;
    return Step::Write;
}

/// The caller wrote `f.slice` (`ok` = erase and write succeeded).
static inline Step wrote(Fetch& f, Progress& p, bool ok) {
    if (!ok || !f.slice) {
        f.imgId   = p.imgId;
        f.attempt = p.attempt;
        return report(f, EN_FW_ST_FLASH_ERROR, 0);
    }
    p.written += f.slice->len;
    f.gained  += f.slice->len;
    f.offset   = p.written;
    f.slice    = nullptr;
    return p.written >= p.size ? Step::Complete : Step::Request;
}

}  // namespace enfw

// ---------------------------------------------------------------------------
// The flash side — FwFetch.cpp (not compiled on the host)
// ---------------------------------------------------------------------------

struct NodeLink;

/// A cold boot (main.cpp's rules): forget any partial download, the refused
/// image and the backoff. RTC memory is garbage then, or another image's.
void fwColdBoot();

/// An ACK flagged EN_ACK_FW_PENDING: run this wake's part of the download,
/// under its backoff. `runningId` is main.cpp's runningImageId(), `vbatMv` the
/// wake's battery reading (0 = none), `mains` picks the budget. Does not return
/// when a new image was staged: it restarts into it.
void fwOnPending(const NodeLink& link, uint32_t runningId, uint16_t vbatMv, bool mains,
                 uint16_t replyWindowMs);
