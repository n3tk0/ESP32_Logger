// ============================================================================
// src/core/EventLog.h — the one diagnostic log on the filesystem
//
// Three places used to open this file by name, each with its own spelling of
// "append a line": the boot reset-reason writer, the OTA event writer, and now
// the ESP-NOW clock-drift warning. One of them wrote to LittleFS while the
// reader in /api/diag read from activeFS, so on a board configured for an SD
// card the OTA lines were written somewhere nothing ever looked.
//
// So the path lives here, once, and so does the append.
//
// THE NAME CHANGED. It was /reset_log.txt, which described what the first
// writer put in it and stopped being true the moment the second one appeared.
// eventLogMigrate() renames an older build's file at boot so the history
// survives the rename rather than being silently orphaned.
// ============================================================================
#pragma once

/// Where the log lives now.
extern const char* const EVENT_LOG_PATH;

/// Where it lived in builds before this one.
extern const char* const EVENT_LOG_LEGACY_PATH;

/// Carry an older build's log onto the current name. Safe to call more than
/// once and safe to call with no filesystem; does nothing if the legacy file is
/// absent, and leaves it alone (rather than merging or deleting) in the case
/// that cannot normally happen, where both files exist.
///
/// Call once at boot, after storage is up and before anything appends.
void eventLogMigrate();

/// Append one line. Adds the newline, takes fsMutex, and is a no-op when there
/// is no filesystem. Lines are capped at 160 characters — this is a diagnostic
/// log on a device with a few hundred kilobytes of flash, not a journal.
///
/// Safe from any task. NOT safe from an ISR or from inside a critical section:
/// it takes a mutex and writes flash.
void eventLogPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
