// ============================================================================
// node_espnow/src/CfgFetch.h
//
// Pulling a new config from the collector inside one wake — docs/NODE_CONFIG.md
// §5 — as a state machine with no radio in it, so tests/host can drive it
// against a simulated collector (tests/host/test_espnow_node_cfgfetch.cpp).
//
// THE SHAPE OF IT
// ---------------
// The collector cannot push: the node is asleep except for the few
// milliseconds after its own DATA frame. So an ACK carrying
// EN_ACK_CFG_PENDING makes the node stay awake a little longer and ask:
//
//     CFG_GET(haveRev, 0) → CFG slice 0..199   → CFG_GET(…, 200) → … → done
//
// one request, one reply, strictly in order (EnCfgAssembler ignores anything
// else). A reply that does not come inside the per-request window is asked for
// again, at most `maxMisses` times in a row; the whole transfer is bounded by
// a wall-clock budget. Running out of either ends the attempt for this wake,
// and the next wake starts again from offset 0 — the contract's rule, and the
// right one, because the desired config may have changed in between.
//
// WHY A BUDGET AND WHAT IT COSTS
// ------------------------------
// The radio in receive is ~85 mA, the single largest draw this node has. A
// document is at most 1024 bytes = 6 slices; with the collector answering
// from its receive callback a slice is a few milliseconds, so a whole fetch
// is typically 10–40 ms — once per config change, i.e. never, in the budget's
// terms. The budget (NODE_CFG_BUDGET_MS, 400 ms) is for the other case: a
// collector that sets the flag and then does not answer. Without a ceiling
// that is a node stuck in receive; with it, a failed attempt costs at most
// 400 ms × 85 mA ≈ 0.009 mAh. And the Backoff below stops that repeating
// every wake: a failed attempt skips the next 1, 3, 7 … 63 wakes' attempts,
// so a collector that never answers costs about 0.2 mAh/day, not 13.
//
// A REJECTED CONFIG IS NOT FETCHED AGAIN
// --------------------------------------
// `rejectedRev` is the rev this node last refused. When the first slice of a
// transfer carries it, the fetch stops there (KnownRejected) and the caller
// repeats its CFG_ACK — one slice, not the whole document, and the collector
// is told again in case the first CFG_ACK was lost.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include "src/espnow/EspNowProto.h"

namespace encfg {

enum class Step : uint8_t {
    Request,        ///< ask (again) with wantRequest()
    Complete,       ///< `a.doc` holds the whole document, rev `a.rev`
    UpToDate,       ///< the collector's rev is the one we have (`rev`)
    KnownRejected,  ///< the collector offers the rev we already refused (`rev`)
};

struct Fetch {
    uint8_t  nodeId;
    uint16_t haveRev;
    uint16_t rejectedRev;   ///< 0 = none
    uint16_t offset;        ///< next offset to ask for
    uint8_t  misses;        ///< consecutive requests with no usable reply
    uint8_t  maxMisses;
    uint16_t rev;           ///< the rev UpToDate / KnownRejected refer to
    uint32_t startMs;
    uint32_t budgetMs;
    EnCfgAssembler a;
};

static inline void begin(Fetch& f, uint8_t nodeId, uint16_t haveRev, uint16_t rejectedRev,
                         uint32_t nowMs, uint32_t budgetMs, uint8_t maxMisses) {
    f.nodeId      = nodeId;
    f.haveRev     = haveRev;
    f.rejectedRev = rejectedRev;
    f.offset      = 0;
    f.misses      = 0;
    f.maxMisses   = maxMisses ? maxMisses : 1;
    f.rev         = 0;
    f.startMs     = nowMs;
    f.budgetMs    = budgetMs;
    espnowCfgReset(f.a);
}

/// Milliseconds left of the budget (0 = spent). Unsigned difference, so it
/// stays right across the millis() wrap.
static inline uint32_t remainingMs(const Fetch& f, uint32_t nowMs) {
    const uint32_t spent = nowMs - f.startMs;
    return spent >= f.budgetMs ? 0 : f.budgetMs - spent;
}

/// The next request to send, or false when this wake's attempt is over
/// (budget spent, or `maxMisses` unanswered in a row).
static inline bool wantRequest(const Fetch& f, uint32_t nowMs, CfgGetMsg& out) {
    if (f.misses >= f.maxMisses) return false;
    if (remainingMs(f, nowMs) == 0) return false;
    espnowFillCfgGet(out, f.nodeId, f.haveRev, f.offset);
    return true;
}

/// Feed what came back for the last request: a CFG frame, or nullptr when
/// the window closed empty.
static inline Step onReply(Fetch& f, const CfgChunkMsg* m) {
    if (!m || m->type != EN_MSG_CFG || m->nodeId != f.nodeId) {
        f.misses++;
        return Step::Request;
    }
    if (m->total == 0) {
        // "Nothing for you": the collector has no newer rev than ours (or has
        // just adopted our local edit as `rev`). Either way, stop asking.
        f.rev = m->rev;
        return Step::UpToDate;
    }
    if (m->offset == 0) {
        // Decide on the FIRST slice, before paying for the rest.
        if (m->rev == f.haveRev) { f.rev = m->rev; return Step::UpToDate; }
        if (f.rejectedRev && m->rev == f.rejectedRev) {
            f.rev = m->rev;
            return Step::KnownRejected;
        }
    }
    switch (espnowCfgFeed(f.a, *m)) {
        case EN_CFG_FEED_DONE:
            return Step::Complete;
        case EN_CFG_FEED_MORE:
            f.offset = f.a.have;
            f.misses = 0;
            return Step::Request;
        default:
            // Not the next slice of this transfer. When it is a slice of a
            // DIFFERENT rev, the desired config changed under us mid-transfer
            // — start that one from 0 now rather than burning the wake on
            // requests for a document that no longer exists. Either way it
            // counts as a miss, so a collector that keeps doing it is bounded.
            if (m->rev != f.a.rev) f.offset = 0;
            f.misses++;
            return Step::Request;
    }
}

// ---------------------------------------------------------------------------
// Backoff — how often a wake may try a config exchange that keeps failing
// ---------------------------------------------------------------------------
// Counted in wakes, like the rescan gate in main.cpp and for the same reason:
// the clock is the one thing on this node that jumps. Level k skips the next
// 2^k - 1 wakes; capped at 6 (63 wakes, about an hour at one a minute).

struct Backoff {
    uint8_t level;
    uint8_t skip;
};

static const uint8_t BACKOFF_MAX_LEVEL = 6;

/// Called once per wake that WOULD try. True = try now.
static inline bool due(Backoff& b) {
    if (b.skip) {
        b.skip--;
        return false;
    }
    return true;
}

static inline void failed(Backoff& b) {
    if (b.level < BACKOFF_MAX_LEVEL) b.level++;
    b.skip = (uint8_t)((1u << b.level) - 1u);
}

static inline void succeeded(Backoff& b) {
    b.level = 0;
    b.skip  = 0;
}

// ---------------------------------------------------------------------------
// Reporting — the other direction, and which exchange a wake makes
// ---------------------------------------------------------------------------
// The node reports its config (CFG_REPORT slices) on the first wake after a
// boot and while it holds a local edit. Either way it waits, after the last
// slice, for the collector's CFG with total == 0 — the only proof the whole
// report was reassembled: the radio's delivery of each slice is not, because
// the collector has ONE assembler and drops another node's report while it
// is busy (nodes that boot together after a power cut report together). No
// answer = report again on a later wake, under the report Backoff, so a
// collector that never answers (one built before it answered the report after
// a boot) costs one report per 64 wakes, not one per wake.

/// Did the frame that came back after a report's last slice (nullptr: none)
/// answer it? For a local report the rev it carries is the one the edit was
/// adopted at, and 0 is not one; for the report after a boot it is the
/// node's own rev handed back, and only its arrival matters.
static inline bool reportAnswered(const CfgChunkMsg* m, uint8_t nodeId, bool local) {
    return m && m->type == EN_MSG_CFG && m->nodeId == nodeId && m->total == 0 &&
           (m->rev || !local);
}

enum class Exchange : uint8_t { None, Report, Fetch };

/// Which config exchange this wake makes, once its DATA was ACKed. Call once
/// per such wake: it consumes the backoffs' skips.
///
///  * A local edit wins: reported before anything is fetched, and nothing is
///    fetched while it waits to be adopted (the adoption answers the pending
///    flag as well).
///  * The report owed since boot goes first when it is due; while its backoff
///    holds it, a pending config is still fetched — so a collector that never
///    answers that report cannot keep a config from arriving.
static inline Exchange choose(bool local, bool reportDue, bool cfgPending, Backoff& report,
                              Backoff& fetch) {
    if (local) return due(report) ? Exchange::Report : Exchange::None;
    if (reportDue && due(report)) return Exchange::Report;
    if (cfgPending && due(fetch)) return Exchange::Fetch;
    return Exchange::None;
}

}  // namespace encfg
