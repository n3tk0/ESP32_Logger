#include "JsonLogger.h"
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
    char line[128];
    int  n = r.toJsonLine(line, sizeof(line));
    if (n <= 0 || n >= (int)sizeof(line)) return;
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
            StaticJsonDocument<192> doc;
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
