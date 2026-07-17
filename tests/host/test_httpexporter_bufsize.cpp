// Host test for the HttpExporter JSON body buffer-size math (bug H1).
//
// HttpExporter.cpp cannot be compiled straight into a host TU cheaply: it
// #includes <HTTPClient.h>, <WiFi.h> and <WiFiClientSecure.h>, none of which
// are provided by tests/host/shims.  So rather than #include the .cpp (the
// pattern used by test_pathutils.cpp), this test reproduces the exact snprintf
// format string and the per-reading byte budget and proves, arithmetically,
// that:
//   1. BYTES_PER_READING covers the true worst-case per-reading output for the
//      maximum field widths declared by the real SensorReading struct, and
//   2. the overflow-proof append/clamp logic never advances past the buffer and
//      always leaves a NUL-terminated string, even when the buffer is far too
//      small.
//
// The real SensorReading struct is pulled in from src/core/SensorTypes.h (via
// the Arduino.h shim) so the field widths stay in lock-step with the firmware.
#include "src/core/SensorTypes.h"
#include "check.h"
#include <cstdio>
#include <cstring>
#include <string>

// Must mirror the constant in src/export/HttpExporter.cpp::send().
static const size_t BYTES_PER_READING = 160;

// The exact format string used by HttpExporter::send() for one reading.
#define READING_FMT \
    "{\"ts\":%lu,\"id\":\"%s\",\"sensor\":\"%s\"," \
    "\"metric\":\"%s\",\"value\":%.4g,\"unit\":\"%s\",\"q\":%u}"

// Build a reading whose char fields are packed to their maximum length and
// whose numeric fields render as wide as plausible, to probe the worst case.
static SensorReading maxReading() {
    SensorReading r;
    r.timestamp = 4294967295UL;                 // widest uint32 -> 10 digits
    memset(r.sensorId,   'X', sizeof(r.sensorId)   - 1);
    memset(r.sensorType, 'Y', sizeof(r.sensorType) - 1);
    memset(r.metric,     'Z', sizeof(r.metric)     - 1);
    memset(r.unit,       'U', sizeof(r.unit)       - 1);
    r.value   = -1234567.0f;                    // wide-ish %.4g rendering
    r.quality = (SensorQuality)255;             // widest %u for a uint8
    return r;
}

// Render one reading into a generous scratch buffer and return the byte count.
static size_t renderLen(const SensorReading& r) {
    char scratch[512];
    int w = snprintf(scratch, sizeof(scratch), READING_FMT,
                     (unsigned long)r.timestamp, r.sensorId, r.sensorType,
                     r.metric, r.value, r.unit, (unsigned)r.quality);
    return (w < 0) ? 0 : (size_t)w;
}

// 1. The per-reading budget covers the worst case with margin.
static void test_budget_covers_worst_case() {
    size_t worst = renderLen(maxReading());
    std::printf("      worst-case reading = %zu bytes, budget = %zu\n",
                worst, BYTES_PER_READING);
    CHECK(worst > 90);                    // the old *90 budget really is too small
    CHECK(worst <= BYTES_PER_READING);    // new budget covers it
}

// 2. Whole-batch allocation is large enough for a full EXPORT_BATCH_SIZE run.
static void test_batch_allocation_fits() {
    const size_t count = 20;              // EXPORT_BATCH_SIZE
    size_t bodyLen = count * BYTES_PER_READING + 32;

    // Total = "[" + count readings + (count-1) commas + "]".
    size_t need = 2;                      // '[' and ']'
    for (size_t i = 0; i < count; i++) {
        need += renderLen(maxReading());
        if (i > 0) need += 1;             // comma
    }
    need += 1;                            // trailing NUL
    std::printf("      batch need = %zu bytes, bodyLen = %zu\n", need, bodyLen);
    CHECK(need <= bodyLen);
}

// Overflow-proof append helper, identical in behaviour to HttpExporter::send().
static bool appendOk(char* body, size_t bodyLen, size_t& pos, int written) {
    size_t remaining = bodyLen - pos;
    if (written < 0 || (size_t)written >= remaining) {
        body[bodyLen - 1] = '\0';
        return false;
    }
    pos += written;
    return true;
}

// 3. With a deliberately tiny buffer the clamp truncates safely: pos never
//    exceeds bodyLen and the buffer stays NUL-terminated (no OOB write).
static void test_clamp_truncates_safely() {
    const size_t count = 20;
    const size_t bodyLen = 48;            // far too small on purpose
    char body[64];
    memset(body, 0xA5, sizeof(body));     // poison to catch missing NUL
    body[bodyLen] = '\0';                 // sentinel just past the logical buffer

    size_t pos = 0;
    bool full = appendOk(body, bodyLen, pos, snprintf(body + pos, bodyLen - pos, "["));
    SensorReading r = maxReading();
    for (size_t i = 0; full && i < count; i++) {
        if (i > 0) {
            full = appendOk(body, bodyLen, pos, snprintf(body + pos, bodyLen - pos, ","));
            if (!full) break;
        }
        full = appendOk(body, bodyLen, pos, snprintf(body + pos, bodyLen - pos, READING_FMT,
            (unsigned long)r.timestamp, r.sensorId, r.sensorType,
            r.metric, r.value, r.unit, (unsigned)r.quality));
    }
    if (full) appendOk(body, bodyLen, pos, snprintf(body + pos, bodyLen - pos, "]"));

    CHECK(pos <= bodyLen);                // never advanced past the buffer
    CHECK(body[bodyLen] == '\0');         // sentinel intact -> no OOB write
    CHECK(strlen(body) < bodyLen);        // logical buffer stays terminated
}

int main() {
    RUN(test_budget_covers_worst_case);
    RUN(test_batch_allocation_fits);
    RUN(test_clamp_truncates_safely);
    return SUMMARY();
}
