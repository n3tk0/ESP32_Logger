#pragma once
#include <Arduino.h>
#include <FS.h>

// ============================================================================
// CsvLogger — wide-CSV append writer with daily rotation and schema guard.
//
// File layout: {dir}/YYYY-MM-DD.csv  (per UTC day; uses epoch-day fallback if
// the timestamp predates 2001-09-09 i.e. "no clock yet").
//
// On the first write to a fresh file, the supplied header line is written.
// Subsequent writes append the row only when the current header line still
// matches the file's existing first line.  If a schema change is detected
// (new sensor came online mid-day), the existing file is rotated to
// `{dir}/YYYY-MM-DD.bak` and a new file is started — guaranteeing every CSV
// has a self-consistent column layout.
//
// All file operations are short-lived (open / append / close) so a power
// loss during a row write loses at most one row.
// ============================================================================
class CsvLogger {
public:
    // Returns true once the log directory exists (or was created).  A false
    // return means the filesystem rejected mkdir (full / read-only / corrupt)
    // and no row will ever be written — the caller MUST disable writing.
    bool begin(fs::FS& fs, const char* dir, uint32_t maxSizeKB);

    // `epoch` is used to pick the file name (UTC day).  `headerLine` is the
    // expected first line of the file (without trailing newline).  `row` is
    // the data row (without trailing newline).  Returns true on success.
    bool appendRow(uint32_t epoch, const char* headerLine, const char* row);

    // Filesystem-backed listing, most recent first (insertion order).  Each
    // name is stripped to the file basename, NUL-terminated.
    int  listFiles(fs::FS& fs, char (*names)[32], int maxNames) const;

    const char* directory() const { return _dir; }

private:
    fs::FS*  _fs        = nullptr;
    char     _dir[33]   = "/logs";
    uint32_t _maxSizeKB = 1024;

    bool _ensureDir();
    void _buildPath(char* pathBuf, size_t len, uint32_t epoch) const;
    void _getDate(uint32_t epoch, char* dateBuf /*[12]*/) const;
    bool _readFirstLine(const char* path, char* buf, size_t bufLen) const;
    void _rotate(const char* path);
    void _enforceSizeRotation(const char* path);
};
