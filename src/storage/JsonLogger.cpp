#include "JsonLogger.h"
#include "../pipeline/AggregationEngine.h"
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
void JsonLogger::begin(fs::FS& fs, const char* logDir,
                        uint32_t maxSizeKB, bool rotateDaily)
{
    _fs           = &fs;
    _maxSizeKB    = maxSizeKB;
    _rotateDaily  = rotateDaily;
    strncpy(_logDir, logDir, sizeof(_logDir)-1);
    _ensureDir();
    Serial.printf("[JsonLogger] dir=%s maxKB=%u rotate=%s\n",
                  _logDir, _maxSizeKB, _rotateDaily ? "daily" : "size");
}

// ---------------------------------------------------------------------------
void JsonLogger::_ensureDir() {
    if (!_fs) return;
    if (!_fs->exists(_logDir)) {
        _fs->mkdir(_logDir);
    }
}

// ---------------------------------------------------------------------------
void JsonLogger::_getCurrentDate(uint32_t ts, char* dateBuf) {
    // dateBuf must be at least 11 bytes ("YYYY-MM-DD\0")
    if (ts < 1000000000UL) {
        // No valid timestamp — use epoch-relative day bucket
        uint32_t day = ts / 86400UL;
        snprintf(dateBuf, 11, "day-%06u", day);
        return;
    }
    time_t t = (time_t)ts;
    struct tm* tm_info = gmtime(&t);
    if (tm_info) {
        strftime(dateBuf, 11, "%Y-%m-%d", tm_info);
    } else {
        snprintf(dateBuf, 11, "%lu", (unsigned long)(ts / 86400UL));
    }
}

// ---------------------------------------------------------------------------
void JsonLogger::_buildPath(char* pathBuf, size_t len, const char* date) {
    snprintf(pathBuf, len, "%s/%s.jsonl", _logDir, date);
}

// ---------------------------------------------------------------------------
void JsonLogger::_rotatIfNeeded(const char* path) {
    if (!_fs || !_fs->exists(path)) return;
    File f = _fs->open(path, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz > (size_t)_maxSizeKB * 1024) {
        // Rename to .jsonl.bak (overwrite previous backup)
        char bakPath[80];
        snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
        _fs->remove(bakPath);
        _fs->rename(path, bakPath);
        Serial.printf("[JsonLogger] rotated %s → %s\n", path, bakPath);
    }
}

// ---------------------------------------------------------------------------
void JsonLogger::write(const SensorReading& r) {
    if (!_fs) return;

    // Format line
    char line[160];
    int  n = r.toJsonLine(line, sizeof(line));
    if (n <= 0 || n >= (int)sizeof(line)) {
        Serial.println("[JsonLogger] WARN: line too long, dropped");
        return;
    }
    line[n] = '\0';

    // Buffer it
    strncpy(_lineBuf[_bufCount], line, sizeof(_lineBuf[0])-1);
    _bufCount++;

    if (_bufCount >= BUF_LINES) flush();
}

// ---------------------------------------------------------------------------
void JsonLogger::flush() {
    if (!_fs || _bufCount == 0) return;

    // Group buffered lines by date so midnight-spanning batches go to the
    // correct file (#10).  Each line carries its own "ts" field.
    _ensureDir();

    // Sort lines into date-bucketed groups (max BUF_LINES = 8, no alloc needed)
    char dates[BUF_LINES][12];
    for (int i = 0; i < _bufCount; i++) {
        dates[i][0] = '\0';
        const char* tsPtr = strstr(_lineBuf[i], "\"ts\":");
        if (tsPtr) {
            uint32_t ts = (uint32_t)strtoul(tsPtr + 5, nullptr, 10);
            _getCurrentDate(ts, dates[i]);
        }
        if (dates[i][0] == '\0') strncpy(dates[i], "unknown", 8);
    }

    // Walk unique dates and write lines belonging to each date in one open/close
    for (int i = 0; i < _bufCount; i++) {
        if (dates[i][0] == '\0') continue;  // already consumed

        char path[80];
        _buildPath(path, sizeof(path), dates[i]);
        _rotatIfNeeded(path);

        File f = _fs->open(path, FILE_APPEND);
        if (!f) {
            Serial.printf("[JsonLogger] Cannot open %s\n", path);
        } else {
            for (int j = i; j < _bufCount; j++) {
                if (strcmp(dates[j], dates[i]) == 0) {
                    f.println(_lineBuf[j]);
                    dates[j][0] = '\0';  // mark consumed
                }
            }
            f.flush();   // fsync equivalent — ensure data survives power loss (#9)
            f.close();
        }
    }

    _bufCount = 0;
}

// ---------------------------------------------------------------------------
// Helper: parse "YYYY-MM-DD" filename prefix to a UTC day range [dayStart, dayEnd).
// Returns false if the name doesn't look like a date file.
static bool _fileDateInRange(const char* name, uint32_t fromTs, uint32_t toTs) {
    // Filename format: "YYYY-MM-DD.jsonl" (LittleFS returns just the base name)
    int y = 0, m = 0, d = 0;
    if (sscanf(name, "%4d-%2d-%2d.jsonl", &y, &m, &d) != 3) return true; // not a date file — include it
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return true;

    // Approximate day start/end in UTC seconds (mktime uses local TZ, add ±1 day buffer)
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
    time_t dayStart = mktime(&t);               // local midnight ≈ UTC midnight ± 14h
    time_t dayEnd   = dayStart + 86400;

    // Skip only when clearly outside range (with 1-day safety margin each side)
    return !((time_t)toTs   < dayStart - 86400 ||
             (time_t)fromTs > dayEnd   + 86400);
}

// ---------------------------------------------------------------------------
size_t JsonLogger::query(fs::FS& fs,
                          uint32_t fromTs, uint32_t toTs,
                          const char* sensorId, const char* metric,
                          SensorReading* out, size_t maxOut)
{
    size_t found = 0;
    File   dir   = fs.open(_logDir);
    if (!dir || !dir.isDirectory()) return 0;

    File entry;
    char lineBuf[160];

    while ((entry = dir.openNextFile()) && found < maxOut) {
        // Only process .jsonl files
        const char* name = entry.name();
        size_t nLen = strlen(name);
        if (nLen < 6 || strcmp(name + nLen - 6, ".jsonl") != 0) {
            entry.close();
            continue;
        }

        // Skip files whose date is clearly outside [fromTs, toTs] (P3 optimisation)
        if (!_fileDateInRange(name, fromTs, toTs)) {
            entry.close();
            continue;
        }

        while (entry.available() && found < maxOut) {
            int len = entry.readBytesUntil('\n', lineBuf, sizeof(lineBuf)-1);
            if (len <= 0) break;
            lineBuf[len] = '\0';

            // Fast parse: extract ts, id, metric, value from JSON line
            // Use ArduinoJson for correctness
            JsonDocument doc;
            if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;

            uint32_t ts = doc["ts"] | 0;
            if (ts < fromTs || ts > toTs) continue;

            if (sensorId && *sensorId) {
                const char* id = doc["id"] | "";
                if (strcmp(id, sensorId) != 0) continue;
            }
            if (metric && *metric) {
                const char* m = doc["metric"] | "";
                if (strcmp(m, metric) != 0) continue;
            }

            SensorReading& r = out[found++];
            r.timestamp = ts;
            strncpy(r.sensorId,   doc["id"]     | "", sizeof(r.sensorId)-1);
            strncpy(r.sensorType, doc["sensor"] | "", sizeof(r.sensorType)-1);
            strncpy(r.metric,     doc["metric"] | "", sizeof(r.metric)-1);
            r.value   = doc["value"]  | 0.0f;
            strncpy(r.unit, doc["unit"] | "", sizeof(r.unit)-1);
            r.quality = (SensorQuality)(doc["q"] | 0);
        }
        entry.close();
    }
    return found;
}

// ---------------------------------------------------------------------------
// streamAggregateQuery — P1/3.1: reads JSONL line-by-line and accumulates
// per-bucket stats without materialising the full raw array.
//
// Memory model: allocates Accum[maxOut] on heap (~80B * 500 = 40KB), writes
// aggregated output directly into out[]. Frees Accum before returning.
// Savings vs two-step query+aggregate: eliminates raw[] (40KB) + agg[] (40KB).
// ---------------------------------------------------------------------------
size_t JsonLogger::streamAggregateQuery(fs::FS& fs,
                                         uint32_t fromTs, uint32_t toTs,
                                         const char* sensorId, const char* metric,
                                         SensorReading* out, size_t maxOut,
                                         TimeBucket bucketMins, AggMode mode,
                                         size_t maxPoints)
{
    // Fall through to regular query for raw mode (no bucketing needed)
    if (bucketMins == BUCKET_RAW || mode == AGG_RAW) {
        return query(fs, fromTs, toTs, sensorId, metric, out, maxOut);
    }

    struct Accum {
        uint32_t bucketTs;     // UTC seconds of bucket start
        double   sum;
        float    vmin, vmax;
        uint32_t count;
        char     metric[16];
        char     unit[12];
        char     sensorId[17];
        char     sensorType[12];
        uint8_t  quality;
    };

    Accum* acc = new Accum[maxOut]();
    if (!acc) return 0;
    size_t nBuckets = 0;

    uint32_t bucketSecs = (uint32_t)bucketMins * 60u;
    if (bucketSecs == 0) bucketSecs = 1;

    char lineBuf[160];
    File dir = fs.open(_logDir);
    if (dir && dir.isDirectory()) {
        File entry;
        while ((entry = dir.openNextFile())) {
            const char* name = entry.name();
            size_t nLen = strlen(name);
            if (nLen < 6 || strcmp(name + nLen - 6, ".jsonl") != 0) {
                entry.close(); continue;
            }
            if (!_fileDateInRange(name, fromTs, toTs)) {
                entry.close(); continue;
            }

            while (entry.available()) {
                int len = entry.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
                if (len <= 0) break;
                lineBuf[len] = '\0';

                JsonDocument doc;
                if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;

                uint32_t ts = doc["ts"] | 0;
                if (ts < fromTs || ts > toTs) continue;
                if (sensorId && *sensorId && strcmp(doc["id"] | "", sensorId) != 0) continue;
                if (metric  && *metric  && strcmp(doc["metric"] | "", metric)   != 0) continue;

                float    val = doc["value"] | 0.0f;
                uint32_t bTs = (ts / bucketSecs) * bucketSecs;

                // Find bucket slot (linear scan; maxOut <= 500 so ≤250 avg ops)
                Accum* slot = nullptr;
                for (size_t i = 0; i < nBuckets; i++) {
                    if (acc[i].bucketTs == bTs) { slot = &acc[i]; break; }
                }
                if (!slot) {
                    if (nBuckets >= maxOut) continue;  // bucket table full
                    slot = &acc[nBuckets++];
                    slot->bucketTs = bTs;
                    slot->sum      = 0;
                    slot->vmin     = val;
                    slot->vmax     = val;
                    slot->count    = 0;
                    strncpy(slot->metric,     doc["metric"] | "", sizeof(slot->metric)-1);
                    strncpy(slot->unit,       doc["unit"]   | "", sizeof(slot->unit)-1);
                    strncpy(slot->sensorId,   doc["id"]     | "", sizeof(slot->sensorId)-1);
                    strncpy(slot->sensorType, doc["sensor"] | "", sizeof(slot->sensorType)-1);
                    slot->quality = (uint8_t)(doc["q"] | 0);
                }
                slot->sum += val;
                slot->count++;
                if (val < slot->vmin) slot->vmin = val;
                if (val > slot->vmax) slot->vmax = val;
            }
            entry.close();
        }
    }

    // Insertion-sort accumulators by bucket start timestamp
    for (size_t i = 1; i < nBuckets; i++) {
        Accum tmp = acc[i];
        size_t j = i;
        while (j > 0 && acc[j-1].bucketTs > tmp.bucketTs) {
            acc[j] = acc[j-1]; j--;
        }
        acc[j] = tmp;
    }

    // Convert accumulators to output SensorReadings
    size_t n = 0;
    for (size_t i = 0; i < nBuckets && n < maxOut; i++) {
        if (acc[i].count == 0) continue;
        SensorReading& r = out[n++];
        r.timestamp = acc[i].bucketTs + bucketSecs / 2;  // bucket midpoint
        switch (mode) {
            case AGG_MIN: r.value = acc[i].vmin; break;
            case AGG_MAX: r.value = acc[i].vmax; break;
            case AGG_SUM: r.value = (float)acc[i].sum;
                          r.timestamp = acc[i].bucketTs + bucketSecs; break;
            default:      r.value = (float)(acc[i].sum / acc[i].count); break;
        }
        strncpy(r.metric,     acc[i].metric,     sizeof(r.metric)-1);
        strncpy(r.unit,       acc[i].unit,        sizeof(r.unit)-1);
        strncpy(r.sensorId,   acc[i].sensorId,   sizeof(r.sensorId)-1);
        strncpy(r.sensorType, acc[i].sensorType, sizeof(r.sensorType)-1);
        r.quality = (SensorQuality)acc[i].quality;
    }

    delete[] acc;

    // Apply LTTB post-bucketing if requested and result exceeds maxPoints
    if (mode == AGG_LTTB && maxPoints > 0 && n > maxPoints) {
        SensorReading* tmp = new SensorReading[maxPoints];
        if (tmp) {
            n = AggregationEngine::lttb(out, n, tmp, maxPoints);
            memcpy(out, tmp, n * sizeof(SensorReading));
            delete[] tmp;
        }
    }

    return n;
}

// ---------------------------------------------------------------------------
int JsonLogger::listFiles(fs::FS& fs, char (*names)[32], int maxNames) {
    int count = 0;
    File dir  = fs.open(_logDir);
    if (!dir || !dir.isDirectory()) return 0;
    File entry;
    while ((entry = dir.openNextFile()) && count < maxNames) {
        strncpy(names[count++], entry.name(), 31);
        entry.close();
    }
    return count;
}
