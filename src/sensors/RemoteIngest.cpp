#include "RemoteIngest.h"

// See TrendRing.cpp: the guard below needs setup.h, which RemoteIngest.h's
// include chain does not reach.
#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <math.h>
#include <string.h>

RemoteIngest remoteIngest;

// Case-sensitive, bounded compare. Node ids and metric names come from a
// JSON body, so both sides are already length-clamped by the copy helpers.
static inline bool eq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static inline void copyClamped(char* dst, size_t dstLen, const char* src) {
    if (src == nullptr) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}

bool RemoteIngest::put(const char* nodeId, const char* metric, float value,
                       const char* unit, uint32_t ts) {
    if (nodeId == nullptr || *nodeId == '\0') return false;
    if (metric == nullptr || *metric == '\0') return false;
    if (!isfinite(value))                     return false;

    const uint32_t now = millis();
    bool stored = false;

    taskENTER_CRITICAL(&_mux);
    int free = -1;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!_e[i].used) {
            if (free < 0) free = i;      // remember the first hole, keep scanning
            continue;
        }
        if (eq(_e[i].nodeId, nodeId) && eq(_e[i].metric, metric)) {
            _e[i].value    = value;
            _e[i].ts       = ts;
            _e[i].rxMillis = now;
            copyClamped(_e[i].unit, MAX_UNIT, unit);
            stored = true;
            break;
        }
    }
    if (!stored && free >= 0) {
        copyClamped(_e[free].nodeId, MAX_NODE_ID, nodeId);
        copyClamped(_e[free].metric, MAX_METRIC,  metric);
        copyClamped(_e[free].unit,   MAX_UNIT,    unit);
        _e[free].value    = value;
        _e[free].ts       = ts;
        _e[free].rxMillis = now;
        _e[free].used     = true;
        stored = true;
    }
    taskEXIT_CRITICAL(&_mux);

    return stored;
}

bool RemoteIngest::putHistorical(const char* nodeId, const char* metric,
                                 float value, const char* unit, uint32_t ts) {
    if (nodeId == nullptr || *nodeId == '\0') return false;
    if (metric == nullptr || *metric == '\0') return false;
    if (!isfinite(value))                     return false;
    // A backdated reading whose date is wrong is worse than a gap: it would be
    // filed under an hour it did not happen in and quietly corrupt the record
    // it was meant to complete.
    if (ts < 1000000000u)                     return false;

    bool dropped = false;

    taskENTER_CRITICAL(&_mux);
    if (_hCount >= MAX_HISTORY) {
        // Full. Drop the OLDEST — losing the start of an outage beats losing
        // the end of it, which is the same rule the node applies to its own
        // buffer, and for the same reason: the recent readings are the ones
        // that say what is happening now.
        _hHead = (_hHead + 1) % MAX_HISTORY;
        _hCount--;
        dropped = true;
    }
    Hist& h = _h[(_hHead + _hCount) % MAX_HISTORY];
    copyClamped(h.nodeId, MAX_NODE_ID, nodeId);
    copyClamped(h.metric, MAX_METRIC,  metric);
    copyClamped(h.unit,   MAX_UNIT,    unit);
    h.value = value;
    h.ts    = ts;
    _hCount++;
    taskEXIT_CRITICAL(&_mux);

    return !dropped;
}

int RemoteIngest::historyPending() const {
    taskENTER_CRITICAL(&_mux);
    const int n = _hCount;
    taskEXIT_CRITICAL(&_mux);
    return n;
}

int RemoteIngest::drain(const char* nodeId, SensorReading* out, int maxOut,
                        uint32_t staleAfterMs) {
    if (nodeId == nullptr || out == nullptr || maxOut <= 0) return 0;

    const uint32_t now = millis();
    int n = 0;

    taskENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_ENTRIES && n < maxOut; i++) {
        if (!_e[i].used || !eq(_e[i].nodeId, nodeId)) continue;

        // millis() wraps every ~49 days; unsigned subtraction is correct
        // across the wrap, which is why age is computed rather than compared.
        const uint32_t age = now - _e[i].rxMillis;

        SensorReading& r = out[n];
        r = SensorReading();
        copyClamped(r.metric, sizeof(r.metric), _e[i].metric);
        copyClamped(r.unit,   sizeof(r.unit),   _e[i].unit);
        r.value     = _e[i].value;
        // Carried through, and it survives: SensorManager stamps only a
        // reading whose timestamp is zero. (It used to be overwritten
        // unconditionally, which is what the old note here said.)
        //
        // This is a mailbox, not a queue, so a slot that is not refilled is
        // re-emitted on every drain with the timestamp it first arrived with.
        // That is deliberate — a node reporting every five minutes should not
        // vanish from the latest-value table between reports — and it is why
        // ProcessingTask's backfill test matters here: once the value is more
        // than two minutes old, its repeats stop being treated as live.
        r.timestamp = _e[i].ts;
        r.quality   = (staleAfterMs > 0 && age > staleAfterMs)
                    ? QUALITY_ERROR
                    : QUALITY_GOOD;
        n++;
    }

    // ── Queued history, oldest first, in whatever room is left ──────────────
    //
    // AFTER the latest values, deliberately. History drains a few readings per
    // tick; ahead of the current value it would leave the live dashboard
    // showing nothing for minutes while an outage's backlog cleared. Behind
    // it, nobody notices.
    //
    // Entries for other nodes are stepped over and kept: each node's plugin
    // drains its own, and a burst from one must not be discarded because
    // another's tick reached the queue first.
    int scanned = 0;
    while (n < maxOut && scanned < _hCount) {
        const int idx = (_hHead + scanned) % MAX_HISTORY;
        if (!eq(_h[idx].nodeId, nodeId)) { scanned++; continue; }

        SensorReading& r = out[n];
        r = SensorReading();
        copyClamped(r.metric, sizeof(r.metric), _h[idx].metric);
        copyClamped(r.unit,   sizeof(r.unit),   _h[idx].unit);
        r.value     = _h[idx].value;
        r.timestamp = _h[idx].ts;      // authoritative; not stamped over
        r.quality   = QUALITY_GOOD;
        n++;

        // Remove it by shifting the entries in front of it back one place.
        // A linear move of at most MAX_HISTORY small structs, and it keeps the
        // queue a simple ring rather than one with holes in it.
        for (int k = scanned; k > 0; k--) {
            const int dst = (_hHead + k) % MAX_HISTORY;
            const int src = (_hHead + k - 1) % MAX_HISTORY;
            _h[dst] = _h[src];
        }
        _hHead = (_hHead + 1) % MAX_HISTORY;
        _hCount--;
    }
    taskEXIT_CRITICAL(&_mux);

    return n;
}

uint32_t RemoteIngest::ageMsForNode(const char* nodeId) const {
    if (nodeId == nullptr) return UINT32_MAX;

    const uint32_t now = millis();
    uint32_t best = UINT32_MAX;

    taskENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!_e[i].used || !eq(_e[i].nodeId, nodeId)) continue;
        const uint32_t age = now - _e[i].rxMillis;
        if (age < best) best = age;
    }
    taskEXIT_CRITICAL(&_mux);

    return best;
}

// Deduplicating the node list is an O(n²) walk of string compares. Doing
// that under taskENTER_CRITICAL would hold interrupts off for far longer
// than put()/drain() do, so the lock is used only to take a linear snapshot
// and the dedup runs on the copy.
int RemoteIngest::_snapshotNodes(char (&out)[MAX_ENTRIES][MAX_NODE_ID]) const {
    int raw = 0;
    taskENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!_e[i].used) continue;
        memcpy(out[raw++], _e[i].nodeId, MAX_NODE_ID);
    }
    taskEXIT_CRITICAL(&_mux);

    // Compact in place, keeping first-seen order.
    int n = 0;
    for (int i = 0; i < raw; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (eq(out[j], out[i])) { dup = true; break; }
        }
        if (!dup && n != i) memcpy(out[n], out[i], MAX_NODE_ID);
        if (!dup) n++;
    }
    return n;
}

int RemoteIngest::nodeCount() const {
    char ids[MAX_ENTRIES][MAX_NODE_ID];
    return _snapshotNodes(ids);
}

bool RemoteIngest::nodeIdAt(int index, char* out, size_t outLen) const {
    if (index < 0 || out == nullptr || outLen == 0) return false;

    char ids[MAX_ENTRIES][MAX_NODE_ID];
    const int n = _snapshotNodes(ids);
    if (index >= n) return false;

    copyClamped(out, outLen, ids[index]);
    return true;
}

#endif  // FEATURE_REMOTE_NODES
