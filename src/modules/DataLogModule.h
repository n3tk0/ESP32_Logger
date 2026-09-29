#pragma once
#include "../core/IModule.h"

// ============================================================================
// DataLogModule — IModule adapter over config.datalog (Pass 5, phase 2).
//
// Mirrors the datalog section of DeviceConfig as JSON for /api/modules/:id.
// /config.bin stays authoritative; saveConfig() keeps modules.json in sync.
// ============================================================================
class DataLogModule : public IModule {
public:
    const char* getId()   const override { return "datalog"; }
    const char* getName() const override { return "Data log"; }
    const char* getDescription() const override {
        return "CSV logging: filename, rotation, retention and column formats.";
    }
    void statusJson(JsonObject out) const override;

    bool load(JsonObjectConst cfg) override;
    bool save(JsonObject cfg)      const override;

    ModuleSchema schema() const override;

    static DataLogModule& instance() { static DataLogModule m; return m; }
};

// A file-name prefix is one path component: 1–32 bytes, no slash, backslash
// or control byte (NUL included — hence the length), and not "." or "..".
// DataLogModule::load() trusts its input, so /save_datalog,
// /api/datalog/create and the settings import all ask this first.
bool datalogPrefixOk(const char* s, size_t n);
