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

    // Determine current date from first buffered line's timestamp
    // (rough: parse "\"ts\":" from line buffer)
    char date[12] = "unknown";

    // All readings in buffer have the same approximate time, use first
    // We extract ts by scanning the first line for "\"ts\":"
    const char* tsPtr = strstr(_lineBuf[0], "\"ts\":");
    if (tsPtr) {
        uint32_t ts = (uint32_t)strtoul(tsPtr + 5, nullptr, 10);
        _getCurrentDate(ts, date);
    }

    char path[80];
    _buildPath(path, sizeof(path), date);
    _rotatIfNeeded(path);
    _ensureDir();

    File f = _fs->open(path, FILE_APPEND);
    if (!f) {
        Serial.printf("[JsonLogger] Cannot open %s\n", path);
        _bufCount = 0;
        return;
    }

    for (int i = 0; i < _bufCount; i++) {
        f.println(_lineBuf[i]);
    }
    f.close();
    _bufCount = 0;
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
