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

struct DatalogCol {
    char sensor[17];
    char metric[16];
    char label[24];
    bool on;                // logged; off = listed but left out
};

// Loads /datalog_cols.json from `fs`. Call once at boot, before the tasks.
void datalogColsBegin(fs::FS& fs);
// Copies the logged columns in file order; returns how many. `rev` changes
// when the list is replaced (the columns may be renumbered), not when
// learn() appends.
int  datalogColsCopy(DatalogCol* out, int max, uint32_t* rev = nullptr);
uint32_t datalogColsRev();
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
// The header line for the current layout and columns; its length or -1.
int  datalogHeader(char* buf, size_t cap);

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
