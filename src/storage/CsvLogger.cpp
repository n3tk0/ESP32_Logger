#include "CsvLogger.h"
#include "../pipeline/LiveAggregator.h"   // ROW_BUF_BYTES — must match aggregator
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
void CsvLogger::begin(fs::FS& fs, const char* dir, uint32_t maxSizeKB) {
    _fs = &fs;
    if (dir && *dir) {
        strncpy(_dir, dir, sizeof(_dir) - 1);
        _dir[sizeof(_dir) - 1] = '\0';
    }
    _maxSizeKB = maxSizeKB ? maxSizeKB : 1024;
    _ensureDir();
    Serial.printf("[CsvLogger] dir=%s maxKB=%lu\n",
                  _dir, (unsigned long)_maxSizeKB);
}

// ---------------------------------------------------------------------------
void CsvLogger::_ensureDir() {
    if (!_fs) return;
    if (!_fs->exists(_dir)) _fs->mkdir(_dir);
}

// ---------------------------------------------------------------------------
void CsvLogger::_getDate(uint32_t epoch, char* dateBuf) const {
    // 1000000000 = 2001-09-09 — anything below is treated as "no clock yet".
    if (epoch < 1000000000UL) {
        uint32_t day = epoch / 86400UL;
        snprintf(dateBuf, 12, "day-%06lu", (unsigned long)day);
        return;
    }
    // gmtime_r: thread-safe variant.  CsvLogger is currently called only from
    // StorageTask but the web UI may share time funcs in future chunks.
    time_t t = (time_t)epoch;
    struct tm tmStorage;
    struct tm* tmInfo = gmtime_r(&t, &tmStorage);
    if (tmInfo) {
        strftime(dateBuf, 12, "%Y-%m-%d", tmInfo);
    } else {
        snprintf(dateBuf, 12, "%lu", (unsigned long)(epoch / 86400UL));
    }
}

// ---------------------------------------------------------------------------
void CsvLogger::_buildPath(char* pathBuf, size_t len, uint32_t epoch) const {
    char dateBuf[12];
    _getDate(epoch, dateBuf);
    snprintf(pathBuf, len, "%s/%s.csv", _dir, dateBuf);
}

// ---------------------------------------------------------------------------
bool CsvLogger::_readFirstLine(const char* path, char* buf, size_t bufLen) const {
    if (!_fs) return false;
    File f = _fs->open(path, "r");
    if (!f) return false;
    int n = f.readBytesUntil('\n', buf, bufLen - 1);
    f.close();
    if (n <= 0) { buf[0] = '\0'; return false; }
    buf[n] = '\0';
    // Strip trailing CR if a previous writer used CRLF
    if (n > 0 && buf[n - 1] == '\r') buf[n - 1] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
void CsvLogger::_rotate(const char* path) {
    if (!_fs) return;
    // path[80] + ".bak" + NUL — give 16 bytes of headroom so future path-
    // length bumps don't silently truncate the backup name.
    char bak[96];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    _fs->remove(bak);             // discard previous .bak if any
    _fs->rename(path, bak);
    Serial.printf("[CsvLogger] rotated %s -> %s\n", path, bak);
}

// ---------------------------------------------------------------------------
void CsvLogger::_enforceSizeRotation(const char* path) {
    if (!_fs || _maxSizeKB == 0) return;
    File f = _fs->open(path, "r");
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz > (size_t)_maxSizeKB * 1024UL) _rotate(path);
}

// ---------------------------------------------------------------------------
bool CsvLogger::appendRow(uint32_t epoch, const char* headerLine, const char* row) {
    if (!_fs || !headerLine || !row) return false;
    _ensureDir();

    char path[80];
    _buildPath(path, sizeof(path), epoch);

    // Schema-change guard: rotate if the existing file's header doesn't
    // match what we're about to write.  Buffer must be at least
    // LiveAggregator::ROW_BUF_BYTES so a long header line doesn't truncate
    // here and cause a spurious schema-mismatch on every append.
    if (_fs->exists(path)) {
        char existing[LiveAggregator::ROW_BUF_BYTES];
        if (_readFirstLine(path, existing, sizeof(existing))) {
            if (strcmp(existing, headerLine) != 0) {
                Serial.println("[CsvLogger] schema change — rotating file");
                _rotate(path);
            }
        }
        _enforceSizeRotation(path);
    }

    bool isNew = !_fs->exists(path);
    File f = _fs->open(path, FILE_APPEND);
    if (!f) {
        Serial.printf("[CsvLogger] open FAILED: %s\n", path);
        return false;
    }

    // Header is written once on file creation.  A short write here would
    // leave a headerless file that future appends would silently corrupt;
    // bail out and remove the empty file so the next call starts clean.
    if (isNew) {
        size_t hwant    = strlen(headerLine);
        size_t hwritten = f.write((const uint8_t*)headerLine, hwant);
        size_t hnl      = f.write((uint8_t)'\n');
        if (hwritten != hwant || hnl == 0) {
            Serial.printf("[CsvLogger] short header write %u/%u — removing %s\n",
                          (unsigned)hwritten, (unsigned)hwant, path);
            f.close();
            _fs->remove(path);
            return false;
        }
    }

    size_t want    = strlen(row);
    size_t written = f.write((const uint8_t*)row, want);
    size_t nl      = f.write((uint8_t)'\n');
    f.flush();
    f.close();

    if (written != want || nl == 0) {
        Serial.printf("[CsvLogger] short row write %u/%u nl=%u (disk full?)\n",
                      (unsigned)written, (unsigned)want, (unsigned)nl);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
int CsvLogger::listFiles(fs::FS& fs, char (*names)[32], int maxNames) const {
    int    count = 0;
    File   dir   = fs.open(_dir);
    if (!dir || !dir.isDirectory()) return 0;
    File entry;
    while ((entry = dir.openNextFile()) && count < maxNames) {
        const char* nm = entry.name();
        size_t      n  = strlen(nm);
        if (n >= 4 && strcmp(nm + n - 4, ".csv") == 0) {
            strncpy(names[count], nm, 31);
            names[count][31] = '\0';
            count++;
        }
        entry.close();
    }
    return count;
}
