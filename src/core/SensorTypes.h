#pragma once
#include <Arduino.h>

// ============================================================================
// SENSOR QUALITY FLAGS
// ============================================================================
enum SensorQuality : uint8_t {
    QUALITY_UNKNOWN   = 0,  // No quality information
    QUALITY_GOOD      = 1,  // Verified reading
    QUALITY_ESTIMATED = 2,  // Interpolated or NTP-fallback timestamp
    QUALITY_ERROR     = 3,  // Sensor error / out of range
};

// ============================================================================
// SENSOR READING
// Atomic unit of data in the pipeline.
// Stored as JSON lines; never modified after write.
// ============================================================================
struct SensorReading {
    uint32_t      timestamp;     // Unix epoch seconds (0 = unknown)
    char          sensorId[17];  // Unique instance id, e.g. "env_indoor"
    char          sensorType[12];// Plugin type,  e.g. "bme280", "sds011"
    char          metric[16];    // Measurement,  e.g. "temperature", "pm25"
    float         value;
    char          unit[12];      // SI/custom,    e.g. "C", "ug/m3", "L/min"
    SensorQuality quality;

    // Zero-initialise
    SensorReading() {
        memset(this, 0, sizeof(*this));
    }

    // Convenience factory used by sensor plugins
    static SensorReading make(uint32_t ts,
                              const char* id,
                              const char* type,
                              const char* metric,
                              float value,
                              const char* unit,
                              SensorQuality q = QUALITY_GOOD)
    {
        SensorReading r;
        r.timestamp = ts;
        strncpy(r.sensorId,   id,     sizeof(r.sensorId)   - 1);
        strncpy(r.sensorType, type,   sizeof(r.sensorType) - 1);
        strncpy(r.metric,     metric, sizeof(r.metric)     - 1);
        r.value   = value;
        strncpy(r.unit,       unit,   sizeof(r.unit)       - 1);
        r.quality = q;
        return r;
    }

    // Serialise to compact JSON line (no trailing newline)
    // Caller provides buffer of at least 128 bytes.
    int toJsonLine(char* buf, size_t bufLen) const {
        return snprintf(buf, bufLen,
            "{\"ts\":%lu,\"id\":\"%s\",\"sensor\":\"%s\","
            "\"metric\":\"%s\",\"value\":%.4g,\"unit\":\"%s\",\"q\":%u}",
            (unsigned long)timestamp, sensorId, sensorType,
            metric, value, unit, (unsigned)quality);
    }
};

// ============================================================================
// AGGREGATION ENUMS  (used by AggregationEngine + /api/data endpoint)
// ============================================================================
enum AggMode : uint8_t {
    AGG_RAW  = 0,   // Pass-through, no reduction
    AGG_AVG  = 1,   // Arithmetic mean per bucket
    AGG_MIN  = 2,   // Minimum per bucket
    AGG_MAX  = 3,   // Maximum per bucket
    AGG_LTTB = 4,   // Largest Triangle Three Buckets (default)
    AGG_SUM  = 5,   // Sum per bucket (rain totals, flow volumes)
};

// Time bucket width in minutes (0 = no bucketing)
enum TimeBucket : uint16_t {
    BUCKET_RAW   = 0,
    BUCKET_1MIN  = 1,
    BUCKET_5MIN  = 5,
    BUCKET_1HOUR = 60,
    BUCKET_1DAY  = 1440,
};

// Parse "5m" / "1h" / "1d" strings into TimeBucket
inline TimeBucket parseBucket(const char* s) {
    if (!s) return BUCKET_5MIN;
    if (strcmp(s, "raw") == 0) return BUCKET_RAW;
    if (strcmp(s, "1m")  == 0) return BUCKET_1MIN;
    if (strcmp(s, "5m")  == 0) return BUCKET_5MIN;
    if (strcmp(s, "1h")  == 0) return BUCKET_1HOUR;
    if (strcmp(s, "1d")  == 0) return BUCKET_1DAY;
    return BUCKET_5MIN;
}

// Parse "lttb" / "avg" / "min" / "max" / "raw" / "sum"
inline AggMode parseMode(const char* s) {
    if (!s) return AGG_LTTB;
    if (strcmp(s, "raw")  == 0) return AGG_RAW;
    if (strcmp(s, "avg")  == 0) return AGG_AVG;
    if (strcmp(s, "min")  == 0) return AGG_MIN;
    if (strcmp(s, "max")  == 0) return AGG_MAX;
    if (strcmp(s, "lttb") == 0) return AGG_LTTB;
    if (strcmp(s, "sum")  == 0) return AGG_SUM;
    return AGG_LTTB;
}
