#pragma once
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include "IModule.h"

// ============================================================================
// ModuleRegistry — central IModule* table (Pass 5, phase 1).
//
// Unlike SensorManager, the registry does NOT own module instances.  Modules
// are long-lived singletons (wifi, ota, theme, …) that register themselves at
// boot.  The registry only holds non-owning pointers and coordinates
// load/save against /config/modules.json.
//
// File layout (phase 2+ will start populating it):
//   /config/modules.json
//   {
//     "version": 1,
//     "modules": {
//       "wifi":  { "enabled": true,  "ssid": "...", ... },
//       "ota":   { "enabled": true,  "autoReboot": false },
//       "theme": { "enabled": true,  "mode": "dark" }
//     }
//   }
//
// Phase 1 ships the table + persistence code; no modules are wired yet so the
// file is never written.  That keeps behaviour bit-identical to pre-Pass-5.
// ============================================================================
class ModuleRegistry {
public:
    static constexpr int MAX_MODULES = 16;
    static constexpr const char* DEFAULT_PATH = "/config/modules.json";

    // Upper bound on /config/modules.json size.  Each module's slice is small
    // (theme is the fattest at ~400 B; wifi ~200 B).  With MAX_MODULES = 16 a
    // 16 KB cap leaves ~5× headroom vs. the realistic worst case and blocks
    // accidentally-huge or maliciously-crafted files from OOMing the parse.
    static constexpr size_t MAX_FILE_BYTES = 16 * 1024;

    // ------------------------------------------------------------------
    // Register a module.  Call before loadAll().  Returns false if the
    // table is full or if a module with the same id is already present.
    // ------------------------------------------------------------------
    bool add(IModule* mod);

    // ------------------------------------------------------------------
    // Read /config/modules.json and dispatch each section to the matching
    // module's load().  Missing sections leave modules at their defaults.
    // Returns true if the file parsed (even if empty).
    // ------------------------------------------------------------------
    bool loadAll(fs::FS& fs, const char* path = DEFAULT_PATH);

    // ------------------------------------------------------------------
    // Aggregate every module's save() into /config/modules.json.
    // Creates the parent directory on demand.  Returns true on success.
    // ------------------------------------------------------------------
    bool saveAll(fs::FS& fs, const char* path = DEFAULT_PATH) const;

    // Call start() on every enabled module.  Logs failures to Serial.
    void startAll();

    // ------------------------------------------------------------------
    // Lookup + iteration
    // ------------------------------------------------------------------
    int       count()            const { return _count; }
    IModule*  get(int idx)       const { return (idx >= 0 && idx < _count) ? _modules[idx] : nullptr; }
    IModule*  getById(const char* id) const;

    // ------------------------------------------------------------------
    // JSON serialisation for /api/modules endpoints.
    //   toIndexJson(arr) → [{id,name,enabled,hasUI}, ...]
    //   toDetailJson(id, obj) → {id,name,enabled,hasUI,config,schema?}
    //     Returns false if `id` is unknown.
    // ------------------------------------------------------------------
    void toIndexJson(JsonArray arr) const;
    bool toDetailJson(const char* id, JsonObject out) const;

private:
    IModule* _modules[MAX_MODULES] = {};
    int      _count = 0;
};

// Global singleton — defined in ModuleRegistry.cpp
extern ModuleRegistry moduleRegistry;
