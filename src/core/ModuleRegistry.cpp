#include "ModuleRegistry.h"
#include <LittleFS.h>

ModuleRegistry moduleRegistry;

// ---------------------------------------------------------------------------
bool ModuleRegistry::add(IModule* mod) {
    if (!mod) return false;
    if (_count >= MAX_MODULES) {
        Serial.println(F("[ModuleRegistry] MAX_MODULES reached"));
        return false;
    }
    if (getById(mod->getId()) != nullptr) {
        Serial.printf("[ModuleRegistry] duplicate id: %s\n", mod->getId());
        return false;
    }
    _modules[_count++] = mod;
    return true;
}

// ---------------------------------------------------------------------------
IModule* ModuleRegistry::getById(const char* id) const {
    if (!id) return nullptr;
    for (int i = 0; i < _count; i++) {
        if (strcmp(_modules[i]->getId(), id) == 0) return _modules[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
bool ModuleRegistry::loadAll(fs::FS& fs, const char* path) {
    if (_count == 0) return true;  // phase-1 no-op when nothing registered

    File f = fs.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[ModuleRegistry] %s not found, using defaults\n", path);
        return true;  // absence is fine — modules keep compile-time defaults
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[ModuleRegistry] parse error: %s\n", err.c_str());
        return false;
    }

    JsonObjectConst modules = doc["modules"].as<JsonObjectConst>();
    if (modules.isNull()) return true;  // empty-but-valid file

    for (int i = 0; i < _count; i++) {
        JsonObjectConst slice = modules[_modules[i]->getId()].as<JsonObjectConst>();
        if (slice.isNull()) continue;
        bool en = slice["enabled"] | true;
        _modules[i]->setEnabled(en);
        _modules[i]->load(slice);
    }
    return true;
}

// ---------------------------------------------------------------------------
bool ModuleRegistry::saveAll(fs::FS& fs, const char* path) const {
    if (_count == 0) return true;  // phase-1 no-op

    // Ensure /config exists (LittleFS auto-creates intermediate dirs on some
    // cores but not reliably — mkdir is cheap if already present).
    fs.mkdir("/config");

    JsonDocument doc;
    doc["version"] = 1;
    JsonObject modules = doc["modules"].to<JsonObject>();
    for (int i = 0; i < _count; i++) {
        JsonObject slice = modules[_modules[i]->getId()].to<JsonObject>();
        slice["enabled"] = _modules[i]->isEnabled();
        _modules[i]->save(slice);
    }

    File f = fs.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[ModuleRegistry] cannot open %s for write\n", path);
        return false;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    return n > 0;
}

// ---------------------------------------------------------------------------
void ModuleRegistry::startAll() {
    for (int i = 0; i < _count; i++) {
        if (!_modules[i]->isEnabled()) continue;
        if (!_modules[i]->start()) {
            Serial.printf("[ModuleRegistry] %s start() requested restart\n",
                          _modules[i]->getId());
        }
    }
}

// ---------------------------------------------------------------------------
void ModuleRegistry::toIndexJson(JsonArray arr) const {
    for (int i = 0; i < _count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]      = _modules[i]->getId();
        o["name"]    = _modules[i]->getName();
        o["enabled"] = _modules[i]->isEnabled();
        o["hasUI"]   = _modules[i]->hasUI();
    }
}

// ---------------------------------------------------------------------------
bool ModuleRegistry::toDetailJson(const char* id, JsonObject out) const {
    IModule* m = getById(id);
    if (!m) return false;
    out["id"]      = m->getId();
    out["name"]    = m->getName();
    out["enabled"] = m->isEnabled();
    out["hasUI"]   = m->hasUI();
    JsonObject cfg = out["config"].to<JsonObject>();
    m->save(cfg);
    const char* s = m->schema();
    if (s) out["schema"] = (const __FlashStringHelper*)s;
    return true;
}
