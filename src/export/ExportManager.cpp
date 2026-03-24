#include "ExportManager.h"
#include <string.h>

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
        DBGF("[ExportManager] %s not found\n", cfgPath);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        DBGF("[ExportManager] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonObject exportCfg = doc["export"].as<JsonObject>();
    if (exportCfg.isNull()) {
        DBGLN("[ExportManager] No 'export' section in config");
        return false;
    }

    int ok = 0;
    for (int i = 0; i < _count; i++) {
        const char* name = _exporters[i]->getName();
        JsonObject  ecfg = exportCfg[name].as<JsonObject>();
        if (!ecfg.isNull() && _exporters[i]->init(ecfg)) {
            // Apply interval_ms from config (cross-cutting, handled centrally)
            uint32_t ivMs = ecfg["interval_ms"] | 0;
            _exporters[i]->setIntervalMs(ivMs);
            ok++;
            DBGF("[ExportManager] '%s' enabled=%s interval=%ums\n",
                          name, _exporters[i]->isEnabled() ? "true" : "false", ivMs);
        }
    }
    // Return true as long as the config parsed successfully — "no exporters
    // enabled" is a valid configuration (e.g. default platform_config.json).
    return true;
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
        DBGF("[ExportManager] '%s' retry %d/%d\n",
                      exp->getName(), attempt + 1, exp->maxRetries());
    }
    // All retries exhausted — spool for later retry (#4.7)
    _spoolBatch(exp, r, n);
    return false;
}

// ---------------------------------------------------------------------------
// _spoolBatch — append failed batch to /spool/<name>.jsonl for later retry.
// Caps spool file at MAX_SPOOL_BYTES to protect flash from runaway growth.
// ---------------------------------------------------------------------------
void ExportManager::_spoolBatch(IExporter* exp,
                                 const SensorReading* r, size_t n) {
    if (!_spoolFS || !r || n == 0) return;

    char path[48];
    snprintf(path, sizeof(path), "/spool/%s.jsonl", exp->getName());

    // Ensure /spool directory exists
    if (!_spoolFS->exists("/spool")) _spoolFS->mkdir("/spool");

    // Size guard: don't grow spool beyond MAX_SPOOL_BYTES
    if (_spoolFS->exists(path)) {
        File sz = _spoolFS->open(path, FILE_READ);
        size_t fSize = sz ? sz.size() : 0;
        if (sz) sz.close();
        if (fSize >= MAX_SPOOL_BYTES) {
            DBGF("[ExportManager] Spool full for '%s' (%zu B) — dropping\n",
                          exp->getName(), fSize);
            return;
        }
    }

    File f = _spoolFS->open(path, FILE_APPEND);
    if (!f) {
        DBGF("[ExportManager] Cannot open spool %s\n", path);
        return;
    }

    char line[160];
    for (size_t i = 0; i < n; i++) {
        int len = r[i].toJsonLine(line, sizeof(line));
        if (len > 0) { f.println(line); }
    }
    f.flush();
    f.close();
    DBGF("[ExportManager] Spooled %zu readings for '%s'\n",
                  n, exp->getName());
}

// ---------------------------------------------------------------------------
// _drainSpool — try to resend readings from spool file. Deletes file on
// complete success; leaves it intact if send fails.
// ---------------------------------------------------------------------------
bool ExportManager::_drainSpool(IExporter* exp) {
    if (!_spoolFS) return true;

    char path[48];
    snprintf(path, sizeof(path), "/spool/%s.jsonl", exp->getName());
    if (!_spoolFS->exists(path)) return true;

    File f = _spoolFS->open(path, FILE_READ);
    if (!f) return true;

    // Read up to one batch at a time to bound memory use
    static constexpr int SPOOL_BATCH = 20;
    SensorReading batch[SPOOL_BATCH];
    int count = 0;
    bool allOk = true;
    char lineBuf[160];

    while (f.available()) {
        int len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
        if (len <= 0) break;
        lineBuf[len] = '\0';

        JsonDocument doc;
        if (deserializeJson(doc, lineBuf) != DeserializationError::Ok) continue;

        SensorReading& sr = batch[count];
        sr.timestamp = doc["ts"] | 0;
        strncpy(sr.sensorId,   doc["id"]     | "", sizeof(sr.sensorId)-1);
        strncpy(sr.sensorType, doc["sensor"] | "", sizeof(sr.sensorType)-1);
        strncpy(sr.metric,     doc["metric"] | "", sizeof(sr.metric)-1);
        sr.value   = doc["value"] | 0.0f;
        strncpy(sr.unit,       doc["unit"]   | "", sizeof(sr.unit)-1);
        sr.quality = (SensorQuality)(doc["q"] | 0);
        count++;

        if (count >= SPOOL_BATCH) {
            if (!exp->send(batch, count)) { allOk = false; break; }
            count = 0;
        }
    }
    f.close();

    if (count > 0 && allOk) allOk = exp->send(batch, count);

    if (allOk) {
        _spoolFS->remove(path);
        DBGF("[ExportManager] Spool drained for '%s'\n", exp->getName());
    }
    return allOk;
}

// ---------------------------------------------------------------------------
void ExportManager::sendAll(const SensorReading* readings, size_t count) {
    static constexpr uint32_t MAX_SENDALL_MS = 30000; // 30s circuit breaker
    uint32_t deadline = millis() + MAX_SENDALL_MS;

    for (int i = 0; i < _count; i++) {
        if (!_exporters[i]->isEnabled()) continue;
        if (millis() > deadline) {
            DBGF("[ExportManager] circuit breaker: skipping '%s'\n",
                          _exporters[i]->getName());
            break;
        }
        // Per-exporter interval_ms throttle (#11)
        uint32_t interval = _exporters[i]->intervalMs();
        uint32_t nowMs = millis();
        if (interval > 0 && (nowMs - _lastSentMs[i]) < interval) continue;

        // Drain any spooled backlog before sending new live data (#4.7)
        _drainSpool(_exporters[i]);

        if (_sendWithRetry(_exporters[i], readings, count)) {
            _lastSentMs[i] = millis();
        }
    }
}

// ---------------------------------------------------------------------------
bool ExportManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}
