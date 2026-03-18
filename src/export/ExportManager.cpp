#include "ExportManager.h"

ExportManager exportManager;

// ---------------------------------------------------------------------------
bool ExportManager::addExporter(IExporter* exporter) {
    if (_count >= MAX_EXPORTERS || !exporter) return false;
    _exporters[_count++] = exporter;
    return true;
}

// ---------------------------------------------------------------------------
bool ExportManager::loadAndInit(fs::FS& fs, const char* cfgPath) {
    File f = fs.open(cfgPath, FILE_READ);
    if (!f) {
        Serial.printf("[ExportManager] %s not found\n", cfgPath);
        return false;
    }

    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[ExportManager] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonObject exportCfg = doc["export"].as<JsonObject>();
    if (exportCfg.isNull()) {
        Serial.println("[ExportManager] No 'export' section in config");
        return false;
    }

    int ok = 0;
    for (int i = 0; i < _count; i++) {
        const char* name = _exporters[i]->getName();
        JsonObject  ecfg = exportCfg[name].as<JsonObject>();
        if (!ecfg.isNull() && _exporters[i]->init(ecfg)) {
            ok++;
            Serial.printf("[ExportManager] '%s' enabled=%s\n",
                          name, _exporters[i]->isEnabled() ? "true" : "false");
        }
    }
    return ok >= 0;
}

// ---------------------------------------------------------------------------
bool ExportManager::_sendWithRetry(IExporter* exp,
                                    const SensorReading* r, size_t n) {
    for (int attempt = 0; attempt <= exp->maxRetries(); attempt++) {
        if (attempt > 0) {
            uint32_t delayMs = exp->retryDelayMs() * (1 << (attempt - 1));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
        if (exp->send(r, n)) return true;
        Serial.printf("[ExportManager] '%s' retry %d/%d\n",
                      exp->getName(), attempt + 1, exp->maxRetries());
    }
    return false;
}

// ---------------------------------------------------------------------------
void ExportManager::sendAll(const SensorReading* readings, size_t count) {
    for (int i = 0; i < _count; i++) {
        if (!_exporters[i]->isEnabled()) continue;
        _sendWithRetry(_exporters[i], readings, count);
    }
}

// ---------------------------------------------------------------------------
bool ExportManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}
