// ============================================================================
// src/utils/MutexGuard.h
//
// RAII wrapper for FreeRTOS SemaphoreHandle_t.
//
// Acquires the mutex on construction with a bounded timeout, releases it on
// destruction. Replaces every manual xSemaphoreTake / xSemaphoreGive pair in
// the codebase to enforce the lock-ordering invariant of REFACTORING_GUIDELINES
// Pillar 1.
//
// Usage:
//   void someFunc() {
//       MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
//       if (!g.isLocked()) return false;     // bail out, do NOT proceed
//       // ... work that holds fsMutex ...
//   }                                        // released here automatically
//
// Contract (Pillar 1.2):
//   - Construction always attempts xSemaphoreTake with the supplied timeout.
//   - portMAX_DELAY is permitted only inside infrastructure code that has
//     been reviewed for liveness; new application code MUST pass a bounded
//     timeout.
//   - On acquisition failure (timeout or null handle), isLocked() returns
//     false. The caller MUST check and fail-safe — proceed-without-lock is
//     FORBIDDEN.
//   - Move-only: ownership transfers; the source becomes "not locked".
//   - Copy: explicitly deleted to prevent double-release.
// ============================================================================
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class MutexGuard {
public:
    /// Default constructor — produces an unlocked, unowned guard.
    /// Use only when you need to declare a guard ahead of acquisition.
    MutexGuard() noexcept : _handle(nullptr), _locked(false) {}

    /// Acquires `handle` with the supplied timeout (in ticks).
    /// Convenience: pass `pdMS_TO_TICKS(ms)` at the call site for clarity.
    ///
    /// If `handle == nullptr`, the guard is constructed in the unlocked state.
    /// Caller MUST check isLocked() before proceeding.
    MutexGuard(SemaphoreHandle_t handle,
               TickType_t timeout = pdMS_TO_TICKS(1000)) noexcept
        : _handle(handle), _locked(false)
    {
        if (_handle != nullptr) {
            _locked = (xSemaphoreTake(_handle, timeout) == pdTRUE);
        }
    }

    /// Releases the mutex if we successfully acquired it.
    ~MutexGuard() noexcept {
        if (_locked && _handle != nullptr) {
            xSemaphoreGive(_handle);
        }
    }

    // ── Move-only semantics ─────────────────────────────────────────────────
    MutexGuard(const MutexGuard&)            = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    MutexGuard(MutexGuard&& other) noexcept
        : _handle(other._handle), _locked(other._locked)
    {
        other._handle = nullptr;
        other._locked = false;
    }

    MutexGuard& operator=(MutexGuard&& other) noexcept {
        if (this != &other) {
            // Release whatever we currently hold before taking the new one.
            if (_locked && _handle != nullptr) {
                xSemaphoreGive(_handle);
            }
            _handle       = other._handle;
            _locked       = other._locked;
            other._handle = nullptr;
            other._locked = false;
        }
        return *this;
    }

    /// True iff the constructor (or attach) successfully acquired the mutex.
    /// Callers MUST check this before performing the protected work.
    bool isLocked() const noexcept { return _locked; }

    /// Explicit early release, primarily for cases where the caller wants to
    /// release the mutex before performing a long blocking I/O operation
    /// (Pillar 1.6) and re-acquire afterwards.
    /// After release(), isLocked() returns false.
    void release() noexcept {
        if (_locked && _handle != nullptr) {
            xSemaphoreGive(_handle);
            _locked = false;
        }
    }

private:
    SemaphoreHandle_t _handle;
    bool              _locked;
};
