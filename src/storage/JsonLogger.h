#pragma once
#include <Arduino.h>
#include <FS.h>
#include "../core/SensorTypes.h"

// ============================================================================
// JsonLogger — writes SensorReadings as JSON lines to daily rotating files.
//
// File path: {logDir}/YYYY-MM-DD.jsonl
// Each line: {"ts":...,"id":...,"sensor":...,"metric":...,"value":...,"unit":..,"q":..}
//
// Raw data is NEVER modified.  Aggregation is read-time only.
// ============================================================================
class JsonLogger {
public:
    // Call once before any write().  Loads config from platform_config.json.
    void begin(fs::FS& fs,
               const char* logDir  = "/logs",
               uint32_t    maxSizeKB = 512,
               bool        rotateDaily = true);

    // Append one reading (buffered; call flush() periodically or on shutdown).
    void write(const SensorReading& r);

    // Force flush write buffer to filesystem.
    void flush();

    // Scan log directory; fill `out` with matching readings.
    // Returns count.  Caller must allocate sufficient `out` buffer.
    size_t query(fs::FS& fs,
                 uint32_t fromTs, uint32_t toTs,
                 const char* sensorId,  // nullptr = all
                 const char* metric,    // nullptr = all
                 SensorReading* out,    size_t maxOut);

    // List available log files (names only, most recent first)
    int listFiles(fs::FS& fs, char (*names)[32], int maxNames);

private:
    fs::FS*  _fs          = nullptr;
    char     _logDir[33]  = "/logs";
    uint32_t _maxSizeKB   = 512;
    bool     _rotateDaily = true;
    char     _currentDate[12] = {}; // "YYYY-MM-DD\0"

    // Write buffer (flush every 8 writes to reduce I/O)
    static constexpr int BUF_LINES = 8;
    char     _lineBuf[BUF_LINES][128];
    int      _bufCount = 0;

    void _ensureDir();
    void _buildPath(char* pathBuf, size_t len, const char* date);
    void _getCurrentDate(uint32_t ts, char* dateBuf);
    void _rotatIfNeeded(const char* path);
};
