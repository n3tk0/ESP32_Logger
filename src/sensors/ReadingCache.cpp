#include "ReadingCache.h"
#include <math.h>
#include <string.h>

ReadingCache readingCache;

// ---------------------------------------------------------------------------
void ReadingCache::put(const SensorReading& r) {
    if (!isfinite(r.value))  return;
    if (r.sensorId[0] == '\0' || r.metric[0] == '\0') return;

    const uint32_t now = millis();

    portENTER_CRITICAL(&_mux);
    int freeSlot = -1;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        Entry& e = _entries[i];
        if (!e.used) {
            if (freeSlot < 0) freeSlot = i;
            continue;
        }
        if (strcmp(e.sensorId, r.sensorId) == 0 && strcmp(e.metric, r.metric) == 0) {
            e.value = r.value;
            e.ms    = now;
            portEXIT_CRITICAL(&_mux);
            return;
        }
    }
    // Not present — claim a slot if one is free. When the table is full we
    // deliberately drop the new series rather than evicting an existing one:
    // eviction would make the heater's staleness check non-deterministic.
    if (freeSlot >= 0) {
        Entry& e = _entries[freeSlot];
        strncpy(e.sensorId, r.sensorId, sizeof(e.sensorId) - 1);
        e.sensorId[sizeof(e.sensorId) - 1] = '\0';
        strncpy(e.metric, r.metric, sizeof(e.metric) - 1);
        e.metric[sizeof(e.metric) - 1] = '\0';
        e.value = r.value;
        e.ms    = now;
        e.used  = true;
    }
    portEXIT_CRITICAL(&_mux);
}

// ---------------------------------------------------------------------------
bool ReadingCache::get(const char* sensorId, const char* metric,
                       float& outValue, uint32_t& outAgeMs) const {
    if (!sensorId || !metric || sensorId[0] == '\0' || metric[0] == '\0') return false;

    const uint32_t now = millis();
    bool found = false;

    portENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_ENTRIES; i++) {
        const Entry& e = _entries[i];
        if (!e.used) continue;
        if (strcmp(e.sensorId, sensorId) == 0 && strcmp(e.metric, metric) == 0) {
            outValue = e.value;
            outAgeMs = now - e.ms;     // unsigned wrap is correct across rollover
            found    = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_mux);
    return found;
}

// ---------------------------------------------------------------------------
void ReadingCache::clear() {
    portENTER_CRITICAL(&_mux);
    for (int i = 0; i < MAX_ENTRIES; i++) _entries[i].used = false;
    portEXIT_CRITICAL(&_mux);
}
