#include "ExportManager.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)
#include "../setup.h"
#include "../pipeline/DataPipeline.h"
#include "../utils/MutexGuard.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../utils/JsonIO.h"
#include "../core/HeapWatch.h"   // HeapActivity

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
        Log.printf("[ExportManager] %s not found\n", cfgPath);
        return false;
    }

    // Cap parse input so a corrupted/crafted config file can't OOM the heap
    // (audit Pass 7 JsonDocument sizing).  Realistic worst case is a couple
    // of KB; 16 KB is comfortably above that while still a hard ceiling.
    constexpr size_t MAX_CFG_BYTES = 16 * 1024;
    if (f.size() > MAX_CFG_BYTES) {
        Log.printf("[ExportManager] %s too large (%u B, cap %u)\n",
                      cfgPath, (unsigned)f.size(), (unsigned)MAX_CFG_BYTES);
        f.close();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJsonFile(doc, f);
    f.close();
    if (err) {
        Log.printf("[ExportManager] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonObject exportCfg = doc["export"].as<JsonObject>();
    if (exportCfg.isNull()) {
        Log.println("[ExportManager] No 'export' section in config");
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
    _parseSensorList(defs["sensors"], DEF_SEL);

    const uint32_t now = millis();
    bool periodic = false;   // anything enabled that reads the latest-value table
    bool streaming = false;  // anything enabled that sends every reading
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
        if (ownSel) _parseSensorList(sel, i);
        _selOf[i] = ownSel ? i : DEF_SEL;

        // Schedule. First time: after min(interval, EXPORT_FIRST_SEND_MS), so
        // the table has had time to collect every sensor and a long interval
        // isn't an equally long silence after boot — but never sooner than
        // the API's own floor: a restart (Save & Restart) right after a post
        // must not let sensor.community see the next one 60 s later. On
        // reload: keep the running schedule, but pull it in if the new
        // interval is shorter.
        if (!_scheduled[i]) {
            uint32_t first = iv < EXPORT_FIRST_SEND_MS ? iv : EXPORT_FIRST_SEND_MS;
            if (first < exp->minIntervalMs()) first = exp->minIntervalMs();
            _nextDueMs[i] = now + first;
            _scheduled[i] = true;
        } else if ((int32_t)(_nextDueMs[i] - (now + iv)) > 0) {
            _nextDueMs[i] = now + iv;
        }

        if (exp->isEnabled() && !exp->isStreaming()) periodic = true;
        if (exp->isEnabled() &&  exp->isStreaming()) streaming = true;
        // sensors: the count selected, 0 = all; '*' = the common setting.
        Log.printf("[ExportManager] '%s' on=%d every %lus%s sensors=%u%s\n",
                      name, (int)exp->isEnabled(), (unsigned long)(iv / 1000),
                      own ? "" : "*", _sensorCount[_selOf[i]], ownSel ? "" : "*");
    }
    _anyStreaming = streaming;
    // The latest-value table is only worth its RAM once something periodic
    // will read it.
    if (periodic && !_latest) {
        // calloc, not new[]: all-zero is exactly an empty table (seq 0, and
        // what SensorReading's own constructor would have memset), without
        // a constructor loop in flash for it.
        _latest = static_cast<LatestSlot*>(calloc(EXPORT_LATEST_SLOTS, sizeof(LatestSlot)));
        if (!_latest) Log.println("[ExportManager] no heap for the latest-value table");
    }

    // Return true as long as the config parsed successfully — "no exporters
    // enabled" is a valid configuration (e.g. default platform_config.json).
    return true;
}

// ---------------------------------------------------------------------------
// Copy a JSON array of sensor ids into selection list `list`. List first,
// count last: see the note on reloads in loadAndInit().
void ExportManager::_parseSensorList(JsonVariantConst v, int list) {
    uint8_t n = 0;
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
        const char* id = e.as<const char*>();
        if (!id || !*id) continue;
        if (n >= EXPORT_MAX_SENSOR_FILTER) break;
        strlcpy(_sensors[list][n++], id, sizeof(_sensors[0][0]));
    }
    _sensorCount[list] = n;
}

// ---------------------------------------------------------------------------
bool ExportManager::_accepts(int idx, const char* sensorId) const {
    const int     list = _selOf[idx];
    const uint8_t n    = _sensorCount[list];
    if (n == 0) return true;                     // empty selection = all
    for (uint8_t k = 0; k < n && k < EXPORT_MAX_SENSOR_FILTER; k++) {
        if (strcmp(_sensors[list][k], sensorId) == 0) return true;
    }
    return false;
}

// A table slot exporter `idx` has yet to send: filled after `since`, by a
// sensor selected for it.
bool ExportManager::_pending(int idx, const LatestSlot& s, uint32_t since) const {
    return s.seq > since && _accepts(idx, s.r.sensorId);
}

// The table sequence every enabled periodic exporter has sent up to.
uint32_t ExportManager::_sentByAll() const {
    uint32_t m = _seq;
    for (int i = 0; i < _count; i++) {
        const IExporter* e = _exporters[i];
        if (e->isEnabled() && !e->isStreaming() && _sentSeq[i] < m) m = _sentSeq[i];
    }
    return m;
}

// ---------------------------------------------------------------------------
// ingest — keep the latest value per sensor+metric; hand streaming exporters
// the reading immediately. Runs on ExportTask only (as does tick()), so the
// table needs no lock.
// ---------------------------------------------------------------------------
void ExportManager::ingest(const SensorReading& r) {
    LatestSlot* const latest = _latest;   // null = no periodic exporter
    // Slots at or below `sentAll` have gone to every periodic exporter, so
    // overwriting one loses nothing; anything above it someone still owes.
    const uint32_t sentAll = _sentByAll();
    int slot = -1, empty = -1, spare = -1, oldest = -1;
    for (int k = 0; latest && k < EXPORT_LATEST_SLOTS; k++) {
        const LatestSlot& s = latest[k];
        if (s.seq == 0) { if (empty < 0) empty = k; continue; }
        if (strcmp(s.r.sensorId, r.sensorId) == 0 &&
            strcmp(s.r.metric,   r.metric)   == 0) { slot = k; break; }
        if (s.seq <= sentAll && (spare < 0 || s.seq < latest[spare].seq)) spare = k;
        if (oldest < 0 || s.seq < latest[oldest].seq) oldest = k;
    }

    bool store = latest != nullptr;
    if (slot >= 0) {
        // A backfilled reading (remote node catching up after an outage) that
        // is older than the value already held is not "the latest". Only
        // while the held one is believable, though: a value stamped ahead of
        // the clock (a node's RTC wrong, then corrected) would otherwise make
        // every later reading "older" and freeze this metric until a reboot.
        // With no wall clock there is nothing to judge by, so take it.
        const SensorReading& cur = latest[slot].r;
        const uint32_t wall = (uint32_t)time(nullptr);
        const bool curPlausible = wall >= 1000000000u && cur.timestamp <= wall + 60;
        if (r.timestamp && cur.timestamp && curPlausible && r.timestamp < cur.timestamp)
            store = false;
    } else {
        // Table full: overwrite a value every exporter already has; only
        // when every slot is still owed to someone, the least recently
        // updated one (and say so — that value is lost).
        slot = (empty >= 0) ? empty : (spare >= 0) ? spare : oldest;
        if (empty < 0 && spare < 0 && slot >= 0)
            Log.printf("[Export] table full, dropped %s/%s\n",
                          latest[slot].r.sensorId, latest[slot].r.metric);
    }
    if (store && slot >= 0) {
        latest[slot].r   = r;
        latest[slot].seq = ++_seq;
    }

    for (int i = 0; i < _count; i++) {
        IExporter* exp = _exporters[i];
        if (!exp->isEnabled() || !exp->isStreaming()) continue;
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
        HeapActivity ha(HA_EXPORT, exp->getName());
        exp->send(&r, 1);
    }
}

// ---------------------------------------------------------------------------
// What exporter `idx` has not been sent yet, of the sensors selected for it,
// copied into a buffer the caller frees. Null with n = 0 when there is none
// (or no heap for it, in which case the values stay unsent for next time).
SensorReading* ExportManager::_buildSnapshot(int idx, size_t& n) {
    LatestSlot* const latest = _latest;
    const uint32_t    since  = _sentSeq[idx];
    SensorReading*    out    = nullptr;
    n = 0;
    if (!latest) return nullptr;
    for (int k = 0; k < EXPORT_LATEST_SLOTS; k++) n += _pending(idx, latest[k], since);
    if (n && !(out = static_cast<SensorReading*>(malloc(n * sizeof(SensorReading))))) {
        n = 0;                        // no heap: stays unsent, tried next time
        return nullptr;
    }
    // j < n: a reload on the web task may change the selection between the
    // two passes, and this one must not write past what the first counted.
    size_t j = 0;
    for (int k = 0; j < n && k < EXPORT_LATEST_SLOTS; k++)
        if (_pending(idx, latest[k], since)) memcpy(&out[j++], &latest[k].r, sizeof(SensorReading));
    n = j;
    _sentSeq[idx] = _seq;
    return out;
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
        HeapActivity ha(HA_EXPORT, exp->getName());
        if (exp->send(r, n)) return true;
        Log.printf("[ExportManager] '%s' retry %d/%d\n",
                      exp->getName(), attempt + 1, maxRetries);
    }
    // All retries exhausted — spool for later retry (#4.7), where replaying
    // makes sense (see IExporter::spoolOnFailure).
    if (exp->spoolOnFailure()) _spoolBatch(exp, r, n);
    return false;
}

// ---------------------------------------------------------------------------
// Spool — failed batches, kept on LittleFS for a later retry (#4.7).
//
// Records are SensorReading as it is in memory, not JSON lines: the JSON
// spool stored the value as "%.4g" (101325 Pa came back as 101300) and paid
// a JsonDocument per line to read it back, about 1.3 KB of flash on a C3
// build that has none to spare. The record size is in the file name, so a
// firmware whose SensorReading has another layout reads none of an old file
// as its own; setSpoolFS() removes the .jsonl files of the old format.
// ---------------------------------------------------------------------------
static void spoolPath(char (&path)[48], const char* name) {
    snprintf(path, sizeof(path), "/spool/%s.r%u", name, (unsigned)sizeof(SensorReading));
}

void ExportManager::setSpoolFS(fs::FS* fs) {
    _spoolFS = fs;
    if (!fs) return;
    char path[48];
    for (int i = 0; i < _count; i++) {
        snprintf(path, sizeof(path), "/spool/%s.jsonl", _exporters[i]->getName());
        if (fs->exists(path)) fs->remove(path);
    }
}

// ---------------------------------------------------------------------------
// _spoolBatch — append a failed batch to the exporter's spool file.
// Caps spool file at MAX_SPOOL_BYTES to protect flash from runaway growth.
// ---------------------------------------------------------------------------
void ExportManager::_spoolBatch(IExporter* exp,
                                 const SensorReading* r, size_t n) {
    if (!_spoolFS || !r || n == 0) return;

    MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
    if (!g.isLocked()) {
        Log.printf("[ExportManager] fsMutex timeout — dropping spool batch for '%s'\n",
                      exp->getName());
        return;
    }

    char path[48];
    spoolPath(path, exp->getName());

    // Ensure /spool directory exists
    if (!_spoolFS->exists("/spool")) _spoolFS->mkdir("/spool");

    File f = _spoolFS->open(path, FILE_APPEND);
    if (f && f.size() % sizeof(SensorReading)) {
        // A torn tail (power lost mid-append): appending after it would
        // misalign every record that follows, so start the file over.
        f.close();
        _spoolFS->remove(path);
        f = _spoolFS->open(path, FILE_APPEND);
    }
    if (!f) {
        Log.printf("[ExportManager] Cannot open spool %s\n", path);
        return;
    }
    // Size guard: don't grow spool beyond MAX_SPOOL_BYTES
    if (f.size() >= MAX_SPOOL_BYTES) {
        Log.printf("[ExportManager] Spool full for '%s' — dropping\n", exp->getName());
    } else {
        const size_t want = n * sizeof(SensorReading);
        if (f.write(reinterpret_cast<const uint8_t*>(r), want) == want) {
            Log.printf("[ExportManager] Spooled %u readings for '%s'\n",
                          (unsigned)n, exp->getName());
        } else {
            // A torn record would put every later append at the wrong offset,
            // and the drain would read them all shifted: fixed-size records
            // have no newline to resync on. File has no truncate, so drop
            // the spool rather than keep a file that is wrong from here on.
            f.close();
            _spoolFS->remove(path);
            Log.printf("[Export] spool write short, dropped %s\n", path);
            return;
        }
    }
    f.close();
}

// ---------------------------------------------------------------------------
// _drainSpool — try to resend readings from spool file. Deletes file on
// complete success; leaves it intact if send fails.
// ---------------------------------------------------------------------------
bool ExportManager::_drainSpool(IExporter* exp) {
    if (!_spoolFS) return true;

    char path[48];
    spoolPath(path, exp->getName());
    if (!_spoolFS->exists(path)) return true;

    File f = _spoolFS->open(path, FILE_READ);
    if (!f) return true;

    // Read up to one batch at a time to bound memory use.
    // EXPORT_SPOOL_BATCH is configured in setup.h.
    SensorReading batch[EXPORT_SPOOL_BATCH];
    bool allOk = true;

    for (;;) {
        size_t got   = f.read(reinterpret_cast<uint8_t*>(batch), sizeof(batch));
        size_t count = got / sizeof(SensorReading);   // a torn tail record is dropped
        if (count == 0) break;
        for (size_t i = 0; i < count; i++) {
            // The file is storage, not a promise: terminate every string and
            // clamp the quality to an enumerator (CM-5) before anyone reads it.
            SensorReading& sr = batch[i];
            sr.sensorId[sizeof(sr.sensorId) - 1]     = '\0';
            sr.sensorType[sizeof(sr.sensorType) - 1] = '\0';
            sr.metric[sizeof(sr.metric) - 1]         = '\0';
            sr.unit[sizeof(sr.unit) - 1]             = '\0';
            if (sr.quality > QUALITY_ERROR) sr.quality = QUALITY_ERROR;
        }
        // Same reason as _sendWithRetry: a spool backlog is many sends in
        // one pass, each as slow as the network is, and the watchdog is
        // counting.
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();
        HeapActivity ha(HA_EXPORT, exp->getName());
        if (!exp->send(batch, count)) { allOk = false; break; }
    }
    f.close();

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
                Log.printf("[ExportManager] Spool drained for '%s'\n", exp->getName());
            } else if (attempt < 4) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        if (!removed) {
            Log.printf("[ExportManager] WARN: sent OK but spool remove failed for '%s' — next cycle will duplicate\n",
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
            Log.printf("[ExportManager] circuit breaker: deferring '%s'\n",
                          exp->getName());
            break;
        }

        // Fixed cadence from the due time, so the schedule doesn't drift by
        // however long each send takes. If we fell a whole interval behind
        // (long outage, breaker), restart the cadence from now instead of
        // firing a burst of catch-up sends.
        // Set for every scheduled exporter by loadAndInit(), never below
        // EXPORT_MIN_INTERVAL_MS.
        const uint32_t iv = exp->intervalMs();
        _nextDueMs[i] += iv;
        if ((int32_t)(now - _nextDueMs[i]) >= 0) _nextDueMs[i] = now + iv;

        size_t n = 0;
        SensorReading* snap = _buildSnapshot(i, n);
        if (!snap) continue;             // none of its sensors reported since last time

        // R14 / AUDIT 11.8: refresh ExportTask heartbeat BEFORE each
        // exporter's network call. With 5 enabled exporters × ~30 s TLS
        // timeout = up to 150 s of blocking I/O inside this loop; without
        // an inner heartbeat refresh the C4 watchdog (30 s) false-positive
        // restarts during legitimate WiFi outages or slow brokers.
        g_taskHeartbeat[TASK_IDX_EXPORT] = millis();

        // Drain any spooled backlog before sending new live data (#4.7)
        if (exp->spoolOnFailure()) _drainSpool(exp);

        _sendWithRetry(exp, snap, n);
        free(snap);
    }
}

// ---------------------------------------------------------------------------
bool ExportManager::reloadConfig(fs::FS& fs, const char* cfgPath) {
    return loadAndInit(fs, cfgPath);
}
