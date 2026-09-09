// ============================================================================
// src/web/IngestBatch.h — how a batch posted to /api/ingest is split up
//
// WHY THIS IS ITS OWN HEADER
// --------------------------
// IngestHandler.cpp needs ESPAsyncWebServer, so nothing in it can be compiled
// on a desktop. The rules below are the part of that file with no I/O in them
// at all — which metric of a batch is the node's current value, which readings
// are history, and how old this endpoint is prepared to believe a reading is —
// and they are also the part where being wrong is silent for weeks. So they
// live here, where tests/host/test_ingest_batch.cpp can reach them.
//
// Every rule here assumes ONE THING about the wire format, and it is worth
// stating plainly because everything else follows from it: a batch arrives
// OLDEST FIRST, with descending ages. That is what the reference node sends
// (node/src/main.cpp walks its ring from at(0)) and what lets the newest
// reading of a metric be found as its last occurrence.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

namespace IngestBatch {

/// The oldest a reading may claim to be: the whole of the trend window, and
/// well past anything storage is asked about as "recent".
///
/// CLAMPED, NOT REJECTED. A node whose millis() has wrapped, or whose queue
/// has been sitting since last week, should still hand its readings over —
/// dated as old as this endpoint is prepared to believe, rather than dropped.
static const uint32_t MAX_AGE_S = 86400u;

inline uint32_t clampAge(uint32_t ageS) {
    return (ageS > MAX_AGE_S) ? MAX_AGE_S : ageS;
}

/// Marks the newest reading of each distinct metric in a batch.
///
/// THE SPLIT THIS DECIDES. The collector holds a node's readings in two
/// places: a mailbox with one slot per (node, metric), which the dashboard,
/// /api/nodes and "last seen" read, and a queue of distinct historical
/// measurements. Getting the split wrong is loud in one direction and silent
/// in the other, and the silent one is worse: send a whole backlog to the
/// mailbox and fourteen of fifteen readings are overwritten microseconds after
/// arriving, but send the NEWEST reading to the queue and a node that is
/// posting perfectly simply reads as offline on every screen, because nothing
/// ever refreshed its slot.
///
/// Walks backwards and takes the first occurrence of each name — which, in a
/// batch ordered oldest first, is the newest. The same split EspNowIngest
/// makes with `i == newest`, arrived at differently because that path knows
/// the sample index and this one has only an array.
///
/// `nameAt(i)` returns the metric name of reading `i`; a null or empty name is
/// skipped, since a reading with no metric goes nowhere in any case. Indices
/// are written to `out` and the count returned, at most `maxOut` — past that
/// the excess is left to the queue, which is where a reading that is not the
/// newest of its metric belongs anyway.
///
/// Templated on the accessor rather than taking an array so the caller does
/// not have to copy a JSON document into one first.
template <typename NameAt>
int findNewestPerMetric(int count, NameAt nameAt, int* out, int maxOut) {
    int n = 0;
    if (out == nullptr || maxOut <= 0) return 0;

    // The names found so far, kept HERE rather than re-read through nameAt().
    //
    // The accessor walks a JSON array, which is a linked list: asking it for
    // element i costs i steps. The inner loop below asks once per name already
    // found, so re-reading turned a forty-eight-reading batch into tens of
    // thousands of link traversals on the web server's own task. Sixteen
    // pointers on the stack is the whole fix, and they stay valid because they
    // point into the document the caller is iterating.
    const char* seenNames[32];
    const int cap = (maxOut < 32) ? maxOut : 32;

    for (int k = count - 1; k >= 0 && n < cap; k--) {
        const char* m = nameAt(k);
        if (m == nullptr || *m == '\0') continue;
        bool seen = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(seenNames[j], m) == 0) { seen = true; break; }
        }
        if (seen) continue;
        seenNames[n] = m;
        out[n++] = k;
    }
    return n;
}

inline bool isNewest(const int* live, int nLive, int idx) {
    for (int j = 0; j < nLive; j++) if (live[j] == idx) return true;
    return false;
}

/// How recent a reading has to be to count as the node's CURRENT value.
///
/// The pipeline's own number: readingIsBackfilled() in SensorTypes.h treats
/// anything older than this as history, so a reading the mailbox called
/// current and the pipeline called backfill would be one the dashboard drew
/// and the alert engine ignored.
static const uint32_t LIVE_AGE_S = 120;

/// Is this reading the node's current value for its metric?
///
/// NEWEST OF ITS METRIC **AND** ACTUALLY RECENT, and the second half is not
/// belt-and-braces. A node handing over an hour-long outage sends it as four
/// batches of forty-eight, oldest first — and "newest in this batch" is true
/// of three readings in every one of those batches, not just the last. Without
/// the age test, batch 1's newest went to the mailbox, batch 2's overwrote it,
/// batch 3's overwrote that: nine of sixty-four buffered samples deleted on
/// arrival, by the very rule that exists to stop the mailbox eating a backlog.
///
/// With it, only the batch that actually carries a fresh reading refreshes the
/// mailbox, and every older sample goes to the queue where it belongs.
inline bool isLive(bool newest, uint32_t ageS) {
    return newest && ageS <= LIVE_AGE_S;
}

/// Does this reading belong in the history queue rather than the mailbox?
///
/// Four conditions, and each one is load-bearing:
///
///   !isLive      the current value of a metric is never history. isLive()
///                says what that means, and why "newest in this batch" alone
///                was not enough.
///   age > 0      an age of zero means "taken now", which is the mailbox.
///   canBackfill  the COLLECTOR'S clock, not the node's. Anchoring an age to a
///                clock that is not set produces a 1970 date, which storage
///                keeps and the chart files under an hour that has not
///                happened. A gap beats a wrong date; the reading is taken as
///                live instead, and dated on arrival.
///   base > age   the same thing again at the arithmetic level: an age that
///                reaches back past the epoch has no timestamp to become.
inline bool isBackfill(bool live, uint32_t ageS, bool canBackfill,
                       uint32_t base) {
    return !live && ageS > 0 && canBackfill && base > ageS;
}

}  // namespace IngestBatch
