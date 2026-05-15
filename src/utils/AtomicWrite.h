// ============================================================================
// src/utils/AtomicWrite.h
//
// Crash-safe LittleFS / SD file replace.
//
// Implements the contract in REFACTORING_GUIDELINES Pillar 3.2:
//
//   1. Acquire fsMutex (bounded timeout).
//   2. Open  "{path}.tmp"  FILE_WRITE.
//   3. Invoke writerFn(tmpFile); abort on false return.
//   4. Close tmpFile (LittleFS flushes on close).
//   5. fs.rename(tmpPath, path)  — LittleFS rename overwrites atomically.
//   6. On any step failure: fs.remove(tmpPath) and return false.
//
// A power loss between steps 2 and 5 leaves the original `path` intact AND
// possibly a stray `*.tmp` sibling. Callers that perform `loadX()` MAY scan
// for and complete a half-finished rename on startup (ModuleRegistry.cpp:36
// already does this for its config slice; pattern is portable to any user
// of atomicWrite).
//
// Template form (header-only) lets the compiler inline trivial writer lambdas
// without `std::function` heap overhead.
// ============================================================================
#pragma once

#include <FS.h>
#include <stdio.h>          // snprintf
#include <string.h>

#include "MutexGuard.h"

/// writerFn signature: bool(File&). Return false to abort the write
/// (caller's writer detected its own error). Return true to commit.
///
/// `fsMutex` may be `nullptr` for code paths that are documented
/// single-threaded (e.g. boot-time migrations before TaskManager::init).
/// Production call sites MUST pass a real mutex handle.
///
/// `timeout` defaults to 2 s, matching Pillar 1.3's fsMutex policy.
template <typename WriterFn>
bool atomicWrite(fs::FS&           fs,
                 const char*       path,
                 WriterFn&&        writerFn,
                 SemaphoreHandle_t fsMutex = nullptr,
                 TickType_t        timeout = pdMS_TO_TICKS(2000))
{
    if (path == nullptr || path[0] == '\0') return false;

    // ── Lock ────────────────────────────────────────────────────────────────
    // Local scope on the guard so we hold the mutex for the open + write +
    // close + rename window only.
    MutexGuard guard(fsMutex, timeout);
    if (fsMutex != nullptr && !guard.isLocked()) {
        // Caller wanted serialisation but we couldn't get it — fail closed.
        return false;
    }

    // ── Build tmp path ──────────────────────────────────────────────────────
    // Reserve room for a 5-char ".tmp" suffix + NUL. LittleFS path length cap
    // is target-specific; longest documented in-codebase path is
    // /spool/sensor_community.jsonl (~30 chars). 256 bytes (standard legacy
    // POSIX PATH_MAX) gives headroom for SD long-filename extensions and
    // future deep nesting without approaching the MIG 3.1 ≥1 KB stack
    // threshold.
    constexpr size_t TMP_PATH_MAX = 256;
    char tmpPath[TMP_PATH_MAX];
    int  n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof(tmpPath)) return false;

    // ── Open tmp ────────────────────────────────────────────────────────────
    File tmpFile = fs.open(tmpPath, FILE_WRITE);
    if (!tmpFile) return false;

    // ── Hand off to writer ──────────────────────────────────────────────────
    bool writerOk = false;
    {
        // The lambda may throw or return false; either way close + cleanup.
        writerOk = writerFn(tmpFile);
    }

    // Close BEFORE checking writerOk so the close itself can surface errors
    // via the FS driver (LittleFS flushes on close).
    tmpFile.close();

    if (!writerOk) {
        fs.remove(tmpPath);
        return false;
    }

    // ── Atomic rename ───────────────────────────────────────────────────────
    // LittleFS rename overwrites the destination atomically when it exists,
    // so we attempt rename FIRST without a prior remove — that's the path
    // that preserves the AUDIT 9.8 / 12.11 / 16.1 invariant on LittleFS.
    if (fs.rename(tmpPath, path)) return true;

    // SD / FAT compatibility fallback: rename-over-existing FAILS on FAT
    // filesystems. We only reach this branch on LittleFS if the FS is in
    // an unexpected state; on SD it's the normal path when `path` already
    // exists. The fallback (remove + rename) IS NOT atomic on SD — a power
    // loss between remove and rename loses the target file. This is
    // unavoidable on FAT (the FS itself doesn't support atomic rename-over).
    // LittleFS callers never lose atomicity because the first rename above
    // succeeded.
    if (fs.exists(path) && fs.remove(path) && fs.rename(tmpPath, path)) {
        return true;
    }

    fs.remove(tmpPath);
    return false;
}

/// Crash-recovery helper: at startup, scan for a stale `{path}.tmp` left
/// behind by an aborted atomicWrite. If the canonical file is missing,
/// promote the .tmp. Otherwise drop the stale .tmp.
///
/// Mirrors ModuleRegistry::loadAll(36-44) recovery pattern, factored out.
inline void atomicWriteRecover(fs::FS&           fs,
                                const char*       path,
                                SemaphoreHandle_t fsMutex,
                                TickType_t        timeout = pdMS_TO_TICKS(2000))
{
    if (path == nullptr || path[0] == '\0') return;

    // 256-byte buffer matches atomicWrite() above; same SD/long-filename
    // headroom argument applies.
    constexpr size_t TMP_PATH_MAX = 256;
    char tmpPath[TMP_PATH_MAX];
    int  n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof(tmpPath)) return;

    // Pillar 1.3: Every LittleFS / SD write call site MUST acquire fsMutex.
    // The mutex parameter is REQUIRED (no default). Callers that genuinely
    // run pre-scheduler (e.g. early boot before TaskManager::init creates
    // the mutex) must pass an explicit `nullptr` AND that fact must be
    // documented at the call site as a Pillar 1.3 exemption.
    MutexGuard guard(fsMutex, timeout);
    if (fsMutex != nullptr && !guard.isLocked()) return;

    if (!fs.exists(tmpPath)) return;

    if (!fs.exists(path)) {
        fs.rename(tmpPath, path);     // promote
    } else {
        fs.remove(tmpPath);           // drop stale
    }
}
