#pragma once
// ============================================================================
// DatalogFormat — the one row format of the data log.
//
// Every writer that puts a row in the data log goes through here: the
// legacy flowmeter run logger (DataLogger.cpp, one row per wake), the
// hybrid fill detector (FlowRunLogger, one row per fill) and StorageTask
// (one row per aggregation interval with the sensor averages). So a file
// has one layout, described by its first line:
//
//   Date|Start|End|Boot|Trigger|Volume|FF|PF|env_indoor_temperature|...
//   29/09/2026|07:31:12|45s|#:123|FF_BTN|L:1,25|FF0|PF1
//   29/09/2026|07:31:00|07:32:00|#:123|TIMER||||21.4|55
//
// The fields up to PF are the original data_log fields, in the original
// order and with the original prefixes (#:, L:, FF, PF), and each is
// present only when the layout turns it on. The sensor columns follow.
// Trailing empty fields are dropped, so a row with no sensor values reads
// exactly like a row of the original format.
//
// Arduino-free, so the host tests can check it (tests/host/test_datalog_format.cpp).
// ============================================================================
#include <stddef.h>
#include <stdint.h>

// Trigger written on the rows StorageTask emits every aggregation interval,
// and on a completed fill from FlowRunLogger. Readers of the water log skip
// DL_TRIGGER_TIMER rows.
#define DL_TRIGGER_TIMER "TIMER"
#define DL_TRIGGER_FLOW  "FLOW"

struct DatalogLayout {
    uint8_t dateFormat;     // DateFormat   (0 = off)
    uint8_t timeFormat;     // TimeFormat
    uint8_t endFormat;      // EndFormat    (2 = off)
    uint8_t volumeFormat;   // VolumeFormat (3 = off) — used when `volume`
    bool    boot;           // #:<bootcount>
    bool    volume;         // a Volume column (a flowmeter is logging runs)
    bool    ff;             // an FF extra-press counter column
    bool    pf;             // a PF extra-press counter column
};

struct DatalogRow {
    uint32_t start;         // UTC epoch; formatted as local time
    uint32_t end;           // UTC epoch; 0 = unknown
    uint16_t boot;
    char     trigger[10];
    float    volume;        // litres; NAN = empty field
    int16_t  ff;            // < 0 = empty field
    int16_t  pf;
};

// Field positions of a layout, for readers. -1 = not in the layout.
struct DatalogFieldIdx {
    int8_t date, start, end, boot, trigger, volume, ff, pf;
    uint8_t nBase;          // fields before the first sensor column
};

DatalogFieldIdx dlFieldIndex(const DatalogLayout& l);

// Header line (no newline). Returns its length, or -1 when it did not fit.
int dlFormatHeader(char* buf, size_t cap, const DatalogLayout& l,
                   const char* const* labels, int nLabels);

// Data row (no newline). `vals` may be null; a NAN value is an empty field.
// Returns its length, or -1 when it did not fit.
int dlFormatRow(char* buf, size_t cap, const DatalogLayout& l,
                const DatalogRow& r, const float* vals, int nVals);

// A header line starts with a field that holds no digit ("Date", "Start");
// every data row starts with a date or a time.
bool dlIsHeaderLine(const char* line);

// A value as the log writes it: at most two decimals, no trailing zeros.
int dlFormatValue(char* buf, size_t cap, float v);
