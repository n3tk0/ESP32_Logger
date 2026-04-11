#pragma once
#include <Arduino.h>
#include <FS.h>
#include "../setup.h"
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

    // Streaming aggregation — reads JSONL files line-by-line and buckets
    // in one pass without materialising raw readings (P1 / 3.1).
    // For AGG_RAW/BUCKET_RAW falls through to query().
    // maxPoints: apply LTTB after bucketing when result > maxPoints (0=no LTTB).
    // Returns number of output readings (always <= maxOut).
    size_t streamAggregateQuery(fs::FS& fs,
                                uint32_t fromTs, uint32_t toTs,
                                const char* sensorId,
                                const char* metric,
                                SensorReading* out,  size_t maxOut,
                                TimeBucket bucketMins,
                                AggMode    mode,
                                size_t     maxPoints = 500);

    // List available log files (names only, most recent first)
    int listFiles(fs::FS& fs, char (*names)[32], int maxNames);

private:
    fs::FS*  _fs          = nullptr;
    char     _logDir[33]  = "/logs";
    uint32_t _maxSizeKB   = 512;
    bool     _rotateDaily = true;
    char     _currentDate[12] = {}; // "YYYY-MM-DD\0"

    // Write buffer — LOG_BUF_LINES is configured in setup.h (default 8).
    static constexpr int BUF_LINES = LOG_BUF_LINES;
    char     _lineBuf[BUF_LINES][160];
    int      _bufCount = 0;

    void _ensureDir();
    void _buildPath(char* pathBuf, size_t len, const char* date);
    void _getCurrentDate(uint32_t ts, char* dateBuf);
    void _rotatIfNeeded(const char* path);
};
