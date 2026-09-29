#pragma once
// ============================================================================
// Datalog — the data log's columns, file and writer, shared by every writer.
//
// One file (Settings > Data log: folder, prefix, rotation, max entries), one
// row format (DatalogFormat.h). Three producers write it:
//   - DataLogger.cpp   legacy flowmeter runs, one row per wake
//   - FlowRunLogger    hybrid fills, one row per fill (trigger FLOW)
//   - StorageTask      the sensor averages, one row per aggregation interval
//                      (trigger TIMER), buffered and written a batch at a time
//
// Sensor columns. /datalog_cols.json lists every metric the log knows of, in
// file order, each logged or switched off:
//   {"auto":true,"cols":[{"s":"env_indoor","m":"temperature","l":"T"},
//                        {"s":"node1","m":"rssi","l":"node1_rssi","off":true}]}
// With "auto" on, a metric of an enabled sensor that is not listed yet is
// appended (logged) the first time a reading of it arrives, and the list is
// saved, so the header does not change from one boot to the next while
// sensors come up. A metric switched off stays listed, so it is not added
// back. A missing file means auto on with nothing listed: every metric.
//
// How a column's readings in one interval become its value: "a" is one of
// avg (the default, and what a column without "a" means), min, max, last or
// sum — {"s":"wind","m":"gust","a":"max"}. Chosen per column because one
// rule is wrong for somebody: a temperature is averaged, a gust is its
// highest, a counter is its last value. A word this firmware does not know
// is read as avg, and saved back as avg.
//
// THE MODE IS IN THE FILE TOO: a column that is not averaged carries it in
// the header after its label, "Gust[max]" (dlFormatHeader). So a change of
// mode changes the header, which starts a new file (datalogAppend), and every
// file says how its own rows were written — the chart reads each one by that,
// not by whatever the column is set to now.
// ============================================================================
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include "DatalogFormat.h"

constexpr uint8_t DL_MAX_COLS    = 24;   // logged columns
constexpr uint8_t DL_MAX_ENTRIES = 32;   // listed metrics, logged or not
// Rows StorageTask collects before one write — the flash sees a batch, not
// every interval. A restart or a sleep writes what is pending first.
constexpr uint8_t DL_BATCH_ROWS = 8;
// Longer than any header line: the base fields (about 50), and DL_MAX_COLS
// columns of a separator, a 23-character label and a mode like "[last]"
// (DatalogFormat.h) — about 790. Every buffer a header is built or read into.
constexpr size_t  DL_HEADER_MAX = 1024;

struct DatalogCol {
    char sensor[17];
    char metric[16];
    char label[24];
    bool on;                // logged; off = listed but left out
    uint8_t agg;            // DatalogAgg (DatalogFormat.h)
};

// Loads /datalog_cols.json from `fs`. Call once at boot, before the tasks.
void datalogColsBegin(fs::FS& fs);
// Copies the logged columns in file order; returns how many. `rev` changes
// when the list is replaced and a column is renumbered or its mode changed,
// not when learn() appends.
int  datalogColsCopy(DatalogCol* out, int max, uint32_t* rev = nullptr);
uint32_t datalogColsRev();
// The DatalogAgg of each logged column, in column order; returns how many.
// `rev` is the revision the modes belong to, read under the same lock.
int  datalogColsAggs(uint8_t* out, int max, uint32_t* rev = nullptr);
// Column of (sensor, metric) among the logged ones, appending it when auto is
// on; -1 when it is off, unknown with auto off, or the list is full.
int  datalogColsLearn(const char* sensor, const char* metric);
// Saves the list if learn() appended to it. The caller holds fsMutex.
void datalogColsSaveIfLearned(fs::FS& fs);
// The list as the settings page and the settings backup see it.
void datalogColsToJson(JsonObject out);
// Replaces the list and saves it (takes fsMutex). false = malformed.
bool datalogColsFromJson(JsonVariantConst v);

// Which base fields the log has on this device now: the formats from
// config.datalog, Volume only with a flowmeter, FF/PF only for a button
// with a pin. `possible` = what the device could log with every option on,
// for the settings page.
DatalogLayout datalogLayout(bool possible = false);
// Whether TIMER rows are written: sensor logging on, and not legacy mode
// (which runs no sensor pipeline). The header carries the sensor columns
// only then.
bool datalogSensorRows();
// The header line for the current layout and columns; its length or -1.
// `rev` is the columns' revision it was built from, read under the same lock.
int  datalogHeader(char* buf, size_t cap, uint32_t* rev = nullptr);
// Whether two row times fall in the same rotation period (always, without
// date rotation or a set clock): a batch is written to one period's file.
bool datalogSamePeriod(uint32_t a, uint32_t b);

// Appends `nLines` newline-terminated rows to the active log file on `fs`,
// after rotating the file when its period, size or header says so and
// trimming it to config.datalog.maxEntries. `epoch` is the newest row's time.
// Returns the rows written in full (a short write stops there), -1 when the
// file could not be opened. The caller holds fsMutex.
int  datalogAppend(fs::FS& fs, const char* header, const char* lines,
                   int nLines, uint32_t epoch);

// Pending rows to the file now. Request is non-blocking (any task);
// FlushAndWait also writes the legacy buffer and waits for StorageTask to
// write its batch, up to `timeoutMs` — for the restart path.
void datalogRequestFlush();
bool datalogFlushRequested();
void datalogFlushDone();
void datalogFlushAndWait(uint32_t timeoutMs);
