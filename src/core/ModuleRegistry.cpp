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

    // Crash-recovery: if a previous saveAll() died between close and rename,
    // a .new sibling is still around.  Complete the rename if the canonical
    // file is missing, otherwise drop the stale tempfile.
    String tmp = String(path) + ".new";
    if (fs.exists(tmp.c_str())) {
        if (!fs.exists(path)) {
            fs.rename(tmp.c_str(), path);
        } else {
            fs.remove(tmp.c_str());
        }
    }

    File f = fs.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[ModuleRegistry] %s not found, using defaults\n", path);
        return true;  // absence is fine — modules keep compile-time defaults
    }

    // Cap the parse input so a corrupted or oversize file can't OOM the device.
    // Realistic worst case is a few KB (see MAX_FILE_BYTES rationale in .h).
    size_t sz = f.size();
    if (sz > MAX_FILE_BYTES) {
        Serial.printf("[ModuleRegistry] %s too large (%u B, cap %u) — ignoring\n",
                      path, (unsigned)sz, (unsigned)MAX_FILE_BYTES);
        f.close();
        return false;
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

    // Crash-safe write: serialize to a sibling .new file, fsync via close,
    // then rename over the real target.  A power loss during write leaves
    // the old file intact; a crash between close and rename leaves a stray
    // .new which the next boot can garbage-collect on demand.
    String tmp = String(path) + ".new";
    File f = fs.open(tmp.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[ModuleRegistry] cannot open %s for write\n", tmp.c_str());
        return false;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    if (n == 0) {
        fs.remove(tmp.c_str());
        return false;
    }
    // LittleFS rename() overwrites the destination atomically when it
    // exists, so we deliberately DON'T remove() first — doing so would
    // create a window where neither file is present if power drops
    // between remove and rename.  The loadAll() recovery path still GC's
    // a stale .new in case rename itself fails mid-operation.
    if (!fs.rename(tmp.c_str(), path)) {
        Serial.printf("[ModuleRegistry] rename %s -> %s failed\n", tmp.c_str(), path);
        fs.remove(tmp.c_str());
        return false;
    }
    return true;
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
