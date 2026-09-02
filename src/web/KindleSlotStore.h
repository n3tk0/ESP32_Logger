// ============================================================================
// src/web/KindleSlotStore.h — loading and saving the dashboard's slot list
//
// A FILE, NOT config.bin, and the split is the one this codebase already makes
// everywhere else: scalar settings — the face, the formats, the refresh
// cadence — live in the binary struct, and lists of configured things —
// sensors, modules, alerts — live in JSON under /config/. The slot list is a
// list of configured things.
//
// The practical difference is that it costs no migration. Every field added to
// DeviceConfig changes sizeof() and puts every deployed device through
// loadConfig()'s migration path, which is exactly the machinery that reset
// everybody's configuration when KindleConfig grew by 33 bytes and the version
// was not bumped. Twelve slots would have been another 600.
//
// Crash-safety and quarantine follow ModuleRegistry: write to .new and rename,
// finish an interrupted rename on the next boot, and move a file that will not
// parse aside rather than dying on it every time the page is opened.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <FS.h>
#include "KindleSlots.h"

#ifndef KINDLE_SLOTS_FILE
#  define KINDLE_SLOTS_FILE "/config/kindle_slots.json"
#endif

/// Cap on the file we will parse. Twelve slots of three short strings comes to
/// well under a kilobyte; four is room for formatting and future fields, and
/// it stops a corrupted length making the parser ask for the heap.
#ifndef KINDLE_SLOTS_MAX_BYTES
#  define KINDLE_SLOTS_MAX_BYTES 4096
#endif

/// Read the slot list.
///
/// An absent file is not an error: it means "never configured", and the caller
/// gets the defaults, which reproduce the dashboard as it was before slots
/// existed. A file that will not parse IS an error, and is renamed aside so
/// the next boot starts clean instead of failing the same way forever.
///
/// `outdoorId`/`indoorId` seed the defaults; they are the existing
/// config.kindle sensor ids.
bool kdSlotsLoad(fs::FS& fs, KindleSlotList& out,
                 const char* outdoorId, const char* indoorId,
                 const char* path = KINDLE_SLOTS_FILE);

/// Write the slot list. Clamps before writing, so a bad list cannot be stored
/// and read back as if it had been checked.
bool kdSlotsSave(fs::FS& fs, const KindleSlotList& list,
                 const char* path = KINDLE_SLOTS_FILE);

/// The list the dashboard is currently drawing. Loaded once at boot and
/// replaced by a successful POST; the renderers read it under no lock because
/// a torn read here costs one frame of one label, and taking a mutex on the
/// AsyncTCP worker to protect that would cost more than it buys.
KindleSlotList& kdSlots();

/// Load into kdSlots() at boot.
void kdSlotsBegin(fs::FS& fs, const char* outdoorId, const char* indoorId);

#endif  // FEATURE_KINDLE_DASHBOARD
