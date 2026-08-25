#include "RemoteIngest.h"

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

int RemoteIngest::drain(const char* nodeId, SensorReading* out, int maxOut,
                        uint32_t staleAfterMs) const {
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
        r.timestamp = _e[i].ts;   // 0 → SensorManager stamps its own clock
        r.quality   = (staleAfterMs > 0 && age > staleAfterMs)
                    ? QUALITY_ERROR
                    : QUALITY_GOOD;
        n++;
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
