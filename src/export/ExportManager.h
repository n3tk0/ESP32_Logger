#pragma once
#include "IExporter.h"
#include <ArduinoJson.h>
#include <FS.h>

// ============================================================================
// ExportManager — registry of exporters + dispatch with retry logic.
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

    // Send batch to all enabled exporters with retry
    void sendAll(const SensorReading* readings, size_t count);

    // Reload config at runtime
    bool reloadConfig(fs::FS& fs,
                      const char* cfgPath = "/platform_config.json");

    int count() const { return _count; }

private:
    IExporter* _exporters[MAX_EXPORTERS] = {};
    int        _count = 0;
    uint32_t   _lastSentMs[MAX_EXPORTERS] = {};  // per-exporter last send time

    bool _sendWithRetry(IExporter* exp,
                        const SensorReading* readings, size_t count);
};

// Global singleton
extern ExportManager exportManager;
