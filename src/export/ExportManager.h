#pragma once
#include "IExporter.h"
#include "../setup.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// ExportManager — registry of exporters + scheduling + dispatch with retry.
//
// WHEN and WHAT is configuration (platform_config.json → "export"):
//
//   "defaults": {                       // common to every exporter
//     "interval_ms": 300000,            // send every 5 min
//     "sensors": ["bme280_1","sds011"]  // sensor ids; [] or absent = all
//   },
//   "opensensemap": {
//     "enabled": true, ...,
//     "interval_ms": 0,                 // absent/0 = use defaults.interval_ms
//     "sensors": ["bme280_1"]           // absent = use defaults.sensors
//   }
//
// Readings are not forwarded one batch at a time any more. ExportTask calls
// ingest() for every reading, which keeps the LATEST value per sensor+metric.
// tick() then, for each exporter whose interval has elapsed, sends the values
// that changed since that exporter's previous send and belong to its selected
// sensors. So a long interval no longer means "whatever happened to be in the
// batch at that moment": every sensor that reported in the window is in it,
// including slow ones whose reading arrived minutes earlier.
//
// Streaming exporters (webhook) bypass all of this and see every reading in
// ingest() — threshold alerts are about the moment, not the interval.
// ============================================================================
class ExportManager {
public:
    static constexpr int MAX_EXPORTERS = 8;

    ~ExportManager() {
        for (int i = 0; i < _count; i++) {
            delete _exporters[i];
            _exporters[i] = nullptr;
        }
    }

    // Register an exporter instance (call before loadAndInit)
    bool addExporter(IExporter* exporter);

    // Load platform_config.json and call init() on each registered exporter
    bool loadAndInit(fs::FS& fs,
                     const char* cfgPath = "/platform_config.json");

    // Feed one reading (ExportTask, for every reading off exportQueue).
    void ingest(const SensorReading& r);

    // Send to every exporter whose interval has elapsed (ExportTask loop).
    void tick();

    // Reload config at runtime
    bool reloadConfig(fs::FS& fs,
                      const char* cfgPath = "/platform_config.json");

    // Set filesystem for spool files (call after the exporters are added) (#4.7)
    void setSpoolFS(fs::FS* fs);

    int count() const { return _count; }

    // True while an enabled exporter sends every reading (Webhook). Only then
    // is a reading that missed exportQueue lost to an exporter; the periodic
    // ones read the latest-value table, where the next reading replaces it.
    bool anyStreaming() const { return _anyStreaming; }
    const char* nameAt(int i) const { return _exporters[i]->getName(); }

private:
    using SensorIdList = char[EXPORT_MAX_SENSOR_FILTER][17];

    IExporter* _exporters[MAX_EXPORTERS] = {};
    int        _count = 0;

    // ── Schedule & selection (per exporter + common defaults) ──────────────
    // Zero until loadAndInit(): any non-zero initialiser here moves the
    // whole object from .bss to .data, i.e. a copy of it into flash.
    uint32_t     _defIntervalMs   = 0;

    // Sensor selections: one list per exporter, and the common one in the
    // last slot (DEF_SEL). _selOf[i] says which list exporter i uses.
    static constexpr int DEF_SEL = MAX_EXPORTERS;
    SensorIdList _sensors[MAX_EXPORTERS + 1]     = {};
    uint8_t      _sensorCount[MAX_EXPORTERS + 1] = {};   // 0 = all sensors
    uint8_t      _selOf[MAX_EXPORTERS]           = {};   // i or DEF_SEL
    uint32_t     _nextDueMs[MAX_EXPORTERS]   = {};
    bool         _scheduled[MAX_EXPORTERS]   = {};
    volatile bool _anyStreaming = false;
    uint32_t     _sentSeq[MAX_EXPORTERS]     = {};   // last table seq sent

    // ── Latest value per sensor+metric ─────────────────────────────────────
    // On the heap, allocated the first time a config enables a periodic
    // exporter and never freed (ExportTask may be reading it while the web
    // task reloads). As a member it was ~9.6 KB of static RAM on every
    // device, most of which export nothing — a lot on a C3.
    struct LatestSlot {
        SensorReading r;
        uint32_t      seq;      // 0 = empty slot (calloc'd)
    };
    LatestSlot* volatile _latest = nullptr;   // EXPORT_LATEST_SLOTS entries
    uint32_t             _seq    = 0;

    fs::FS*  _spoolFS  = nullptr;
    static constexpr uint32_t MAX_SPOOL_BYTES = 32768;  // 32 KB per exporter

    bool   _accepts(int idx, const char* sensorId) const;
    bool   _pending(int idx, const LatestSlot& s, uint32_t since) const;
    uint32_t _sentByAll() const;
    SensorReading* _buildSnapshot(int idx, size_t& n);
    void   _parseSensorList(JsonVariantConst v, int list);

    bool _sendWithRetry(IExporter* exp,
                        const SensorReading* readings, size_t count);
    void _spoolBatch(IExporter* exp,
                     const SensorReading* readings, size_t count);
    bool _drainSpool(IExporter* exp);
};

// Global singleton
extern ExportManager exportManager;
