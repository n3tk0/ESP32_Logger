#pragma once
#include "IExporter.h"
#include <HTTPClient.h>

// ============================================================================
// HttpExporter — POSTs readings to any HTTP endpoint as JSON array.
//
// Config keys: url, method ("POST"), headers (object), interval_ms
// Body format: array of SensorReading JSON objects
// ============================================================================
class HttpExporter : public IExporter {
public:
    bool        init(JsonObjectConst config) override;
    bool        send(const SensorReading* readings, size_t count) override;
    const char* getName()   const override { return "http"; }
    bool        isEnabled() const override { return _enabled; }

private:
    char     _url[129]    = {};
    char     _method[8]   = "POST";
    // Headers stored as flat key=value pairs (max 4 headers)
    char     _hdrKeys[4][33]  = {};
    char     _hdrVals[4][129] = {};
    int      _hdrCount    = 0;
};
