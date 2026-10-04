#include "ExportManager.h"
#include "../setup.h"
#include "../pipeline/DataPipeline.h"
#include "../utils/MutexGuard.h"
#include <string.h>
#include "../utils/JsonIO.h"

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

    // Cap parse input so a corrupted/crafted config file can't OOM the heap
    // (audit Pass 7 JsonDocument sizing).  Realistic worst case is a couple
    // of KB; 16 KB is comfortably above that while still a hard ceiling.
    constexpr size_t MAX_CFG_BYTES = 16 * 1024;
    if (f.size() > MAX_CFG_BYTES) {
        Serial.printf("[ExportManager] %s too large (%u B, cap %u)\n",
                      cfgPath, (unsigned)f.size(), (unsigned)MAX_CFG_BYTES);
        f.close();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJsonFile(doc, f);
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

    // ── Common defaults: export.defaults.{interval_ms, sensors} ───────────
    // Written list-first, count-last: reloadConfig() runs on the web task
    // while ExportTask may be reading these, and a reader that sees the old
    // count against a half-copied list mis-filters at most one send. Nothing
    // here is freed or resized, so there is no worse outcome than that.
    JsonObjectConst defs = exportCfg["defaults"].as<JsonObjectConst>();
    uint32_t defIv = defs["interval_ms"] | (uint32_t)EXPORT_FLUSH_INTERVAL_MS;
    if (defIv == 0)                     defIv = EXPORT_FLUSH_INTERVAL_MS;
    if (defIv < EXPORT_MIN_INTERVAL_MS) defIv = EXPORT_MIN_INTERVAL_MS;
    _defIntervalMs  = defIv;
    _defSensorCount = _parseSensorList(defs["sensors"], _defSensors);
    Serial.printf("[ExportManager] defaults: interval=%lus sensors=%s\n",
                  (unsigned long)(_defIntervalMs / 1000),
                  _defSensorCount ? "selected" : "all");

    const uint32_t now = millis();
    for (int i = 0; i < _count; i++) {
        IExporter*  exp  = _exporters[i];
        const char* name = exp->getName();
        JsonObject  ecfg = exportCfg[name].as<JsonObject>();
        if (ecfg.isNull() || !exp->init(ecfg)) continue;

        // Interval: own interval_ms, absent/0 → the common one; never below
        // the global floor or what the remote API demands.
        uint32_t own = ecfg["interval_ms"] | 0;
        uint32_t iv  = own ? own : _defIntervalMs;
        if (iv < EXPORT_MIN_INTERVAL_MS) iv = EXPORT_MIN_INTERVAL_MS;
        if (iv < exp->minIntervalMs())   iv = exp->minIntervalMs();
        exp->setIntervalMs(iv);

        // Sensors: an own "sensors" array (even empty = all) overrides the
        // common list; no key at all means "use the common list".
        JsonVariantConst sel = ecfg["sensors"];
        bool ownSel = sel.is<JsonArrayConst>();
        if (ownSel) _sensorCount[i] = _parseSensorList(sel, _sensors[i]);
        _ownSensors[i] = ownSel;

        // Schedule. First time: after min(interval, EXPORT_FIRST_SEND_MS), so
        // the table has had time to collect every sensor and a long interval
        // isn't an equally long silence after boot. On reload: keep the
        // running schedule, but pull it in if the new interval is shorter.
        if (!_scheduled[i]) {
            _nextDueMs[i] = now + (iv < EXPORT_FIRST_SEND_MS ? iv : EXPORT_FIRST_SEND_MS);
            _scheduled[i] = true;
        } else if ((int32_t)(_nextDueMs[i] - (now + iv)) > 0) {
            _nextDueMs[i] = now + iv;
        }

        uint8_t nSel = ownSel ? _sensorCount[i] : _defSensorCount;
        Serial.printf("[ExportManager] '%s' enabled=%s interval=%lus%s sensors=%s%s\n",
                      name, exp->isEnabled() ? "true" : "false",
                      (unsigned long)(iv / 1000), own ? "" : " (common)",
                      nSel ? "selected" : "all", ownSel ? "" : " (common)");
    }
    // Return true as long as the config parsed successfully — "no exporters
    // enabled" is a valid configuration (e.g. default platform_config.json).
    return true;
}

// ---------------------------------------------------------------------------
uint8_t ExportManager::_parseSensorList(JsonVariantConst v, SensorIdList& out) {
    JsonArrayConst arr = v.as<JsonArrayConst>();
    if (arr.isNull()) return 0;
    uint8_t n = 0;
    for (JsonVariantConst e : arr) {
        if (n >= EXPORT_MAX_SENSOR_FILTER) break;
        const char* id = e.as<const char*>();
        if (!id || !*id) continue;
        strncpy(out[n], id, sizeof(out[n]) - 1);
        out[n][sizeof(out[n]) - 1] = '\0';
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
bool ExportManager::_accepts(int idx, const char* sensorId) const {
    const bool         own  = _ownSensors[idx];
    const uint8_t      n    = own ? _sensorCount[idx] : _defSensorCount;
    const SensorIdList& lst = own ? _sensors[idx]     : _defSensors;
    if (n == 0) return true;                     // empty selection = all
    for (uint8_t k = 0; k < n && k < EXPORT_MAX_SENSOR_FILTER; k++) {
        if (strcmp(lst[k], sensorId) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ingest — keep the latest value per sensor+metric; hand streaming exporters
// the reading immediately. Runs on ExportTask only (as does tick()), so the
// table needs no lock.
// ---------------------------------------------------------------------------
void ExportManager::ingest(const SensorReading& r) {
    int slot = -1, empty = -1, oldest = -1;
    for (int k = 0; k < EXPORT_LATEST_SLOTS; k++) {
        const LatestSlot& s = _latest[k];
        if (s.seq == 0) { if (empty < 0) empty = k; continue; }
        if (strcmp(s.r.sensorId, r.sensorId) == 0 &&
            strcmp(s.r.metric,   r.metric)   == 0) { slot = k; break; }
        if (oldest < 0 || s.seq < _latest[oldest].seq) oldest = k;
    }

    bool store = true;
    if (slot >= 0) {
        // A backfilled reading (remote node catching up after an outage) that
        // is older than the value already held is not "the latest".
        const SensorReading& cur = _latest[slot].r;
        if (r.timestamp && cur.timestamp && r.timestamp < cur.timestamp) store = false;
    } else {
        // Table full: evict the least recently updated metric.
        slot = (empty >= 0) ? empty : oldest;
    }
    if (store && slot >= 0) {
        _latest[slot].r   = r;
        _latest[slot].seq = ++_seq;
    }

    for (int i = 0; i < _count; i++) {
        IExporter* exp = _exporters[i];
        if (!exp->isEnabled() || !exp->isStreaming()) continue;
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
        exp->send(&r, 1);
    }
}

// ---------------------------------------------------------------------------
size_t ExportManager::_buildSnapshot(int idx) {
    size_t   n     = 0;
    uint32_t since = _sentSeq[idx];
    for (int k = 0; k < EXPORT_LATEST_SLOTS; k++) {
        const LatestSlot& s = _latest[k];
        if (s.seq <= since) continue;               // empty or already sent
        if (!_accepts(idx, s.r.sensorId)) continue; // not selected for this exporter
        _snap[n++] = s.r;
    }
    _sentSeq[idx] = _seq;
    return n;
}

// ---------------------------------------------------------------------------
bool ExportManager::_sendWithRetry(IExporter* exp,
                                    const SensorReading* r, size_t n) {
    // CM-3: maxRetries() is uint8_t — promote it explicitly to int so the
    // loop bound comparison is unambiguously signed/signed.
    const int maxRetries = (int)exp->maxRetries();
    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        // THE HEARTBEAT HAS TO BE FED IN HERE, not only once per exporter in
        // sendAll(). The defaults are maxRetries 3 and retryDelayMs 1000, so
        // this loop is four send() attempts plus 1+2+4 s of backoff, and none
        // of the HTTP exporters set a timeout — HTTPClient's own default is
        // 5 s per attempt, and a TLS handshake to a host that is simply not
        // answering takes longer than that.
        //
        // 4 x 5 s + 7 s of backoff is 27 s against a MAX_SILENCE_MS of 30 s
        // (TaskManager::checkHealth), for ONE unreachable exporter. So a slow
        // or blackholed upstream server used to reboot the logger: the
        // watchdog saw ExportTask silent, set shouldRestart, and the device
        // restarted mid-export with nothing wrong with it.
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
        if (attempt > 0) {
            uint32_t delayMs = exp->retryDelayMs() * (1 << (attempt - 1));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
        }
        if (exp->send(r, n)) return true;
        Serial.printf("[ExportManager] '%s' retry %d/%d\n",
                      exp->getName(), attempt + 1, maxRetries);
    }
    // All retries exhausted — spool for later retry (#4.7), where replaying
    // makes sense (see IExporter::spoolOnFailure).
    if (exp->spoolOnFailure()) _spoolBatch(exp, r, n);
    return false;
}

// ---------------------------------------------------------------------------
// _spoolBatch — append failed batch to /spool/<name>.jsonl for later retry.
// Caps spool file at MAX_SPOOL_BYTES to protect flash from runaway growth.
// ---------------------------------------------------------------------------
void ExportManager::_spoolBatch(IExporter* exp,
                                 const SensorReading* r, size_t n) {
    if (!_spoolFS || !r || n == 0) return;

    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (!g.isLocked()) {
        Serial.printf("[ExportManager] fsMutex timeout — dropping spool batch for '%s'\n",
                      exp->getName());
        return;
    }

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
            Serial.printf("[ExportManager] Spool full for '%s' (%zu B) — dropping\n",
                          exp->getName(), fSize);
            return;
        }
    }

    File f = _spoolFS->open(path, FILE_APPEND);
    if (!f) {
        Serial.printf("[ExportManager] Cannot open spool %s\n", path);
        return;
    }

    char line[160];
    for (size_t i = 0; i < n; i++) {
        int len = r[i].toJsonLine(line, sizeof(line));
        if (len > 0) { f.println(line); }
    }
    f.flush();
    f.close();
    Serial.printf("[ExportManager] Spooled %zu readings for '%s'\n",
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

    // Read up to one batch at a time to bound memory use.
    // EXPORT_SPOOL_BATCH is configured in setup.h.
    SensorReading batch[EXPORT_SPOOL_BATCH];
    int count = 0;
    bool allOk = true;
    char lineBuf[160];

    while (f.available()) {
        int len = f.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
        if (len <= 0) break;
        lineBuf[len] = '\0';

        JsonDocument doc;
        if (deserializeJson(doc, (const char*)lineBuf, len) != DeserializationError::Ok) continue;

        SensorReading& sr = batch[count];
        sr.timestamp = doc["ts"] | 0;
        strncpy(sr.sensorId,   doc["id"]     | "", sizeof(sr.sensorId)-1);
        strncpy(sr.sensorType, doc["sensor"] | "", sizeof(sr.sensorType)-1);
        strncpy(sr.metric,     doc["metric"] | "", sizeof(sr.metric)-1);
        sr.value   = doc["value"] | 0.0f;
        strncpy(sr.unit,       doc["unit"]   | "", sizeof(sr.unit)-1);
        // CM-5: clamp untrusted spool-file value to a defined enumerator
        // (0..QUALITY_ERROR) instead of blindly casting an arbitrary int.
        int q = doc["q"] | 0;
        if (q < 0 || q > QUALITY_ERROR) q = QUALITY_ERROR;
        sr.quality = (SensorQuality)q;
        count++;

        if (count >= EXPORT_SPOOL_BATCH) {
            // Same reason as _sendWithRetry: a spool backlog is many sends in
            // one pass, each as slow as the network is, and the watchdog is
            // counting.
            g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
            if (!exp->send(batch, count)) { allOk = false; break; }
            count = 0;
        }
    }
    f.close();

    if (count > 0 && allOk) allOk = exp->send(batch, count);

    if (allOk) {
        // We've already sent — failing to remove here means the next drain
        // cycle will re-send (duplicate delivery). Retry with backoff before
        // giving up.
        bool removed = false;
        for (int attempt = 0; attempt < 5 && !removed; attempt++) {
            MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
            if (g.isLocked()) {
                _spoolFS->remove(path);
                removed = true;
                Serial.printf("[ExportManager] Spool drained for '%s'\n", exp->getName());
            } else if (attempt < 4) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        if (!removed) {
            Serial.printf("[ExportManager] WARN: sent OK but spool remove failed for '%s' — next cycle will duplicate\n",
                          exp->getName());
            return false;
        }
    }
    return allOk;
}

// ---------------------------------------------------------------------------
// tick — called from the ExportTask loop (~10 Hz). Cheap when nothing is due.
// ---------------------------------------------------------------------------
void ExportManager::tick() {
    // EXPORT_MAX_SENDALL_MS is the per-call circuit breaker (setup.h).
    const uint32_t start = millis();

    for (int i = 0; i < _count; i++) {
        IExporter* exp = _exporters[i];
        if (!exp->isEnabled() || exp->isStreaming() || !_scheduled[i]) continue;

        const uint32_t now = millis();
        if ((int32_t)(now - _nextDueMs[i]) < 0) continue;      // not due yet

        if (now - start > EXPORT_MAX_SENDALL_MS) {
            // Unlike the old sendAll(), nothing is lost: this exporter is
            // still due and its values are still in the table next tick.
            Serial.printf("[ExportManager] circuit breaker: deferring '%s'\n",
                          exp->getName());
            break;
        }

        // Fixed cadence from the due time, so the schedule doesn't drift by
        // however long each send takes. If we fell a whole interval behind
        // (long outage, breaker), restart the cadence from now instead of
        // firing a burst of catch-up sends.
        const uint32_t iv = exp->intervalMs() ? exp->intervalMs() : _defIntervalMs;
        _nextDueMs[i] += iv;
        if ((int32_t)(now - _nextDueMs[i]) >= 0) _nextDueMs[i] = now + iv;

        size_t n = _buildSnapshot(i);
        if (n == 0) continue;            // none of its sensors reported since last time

        // R14 / AUDIT 11.8: refresh ExportTask heartbeat BEFORE each
        // exporter's network call. With 5 enabled exporters × ~30 s TLS
        // timeout = up to 150 s of blocking I/O inside this loop; without
        // an inner heartbeat refresh the C4 watchdog (30 s) false-positive
        // restarts during legitimate WiFi outages or slow brokers.
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();

        // Drain any spooled backlog before sending new live data (#4.7)
        if (exp->spoolOnFailure()) _drainSpool(exp);

        _sendWithRetry(exp, _snap, n);
    }
}

// ---------------------------------------------------------------------------
bool ExportManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}
