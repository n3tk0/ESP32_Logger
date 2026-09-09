// ============================================================================
// src/pipeline/TrendStore.h — the 24-hour chart, kept across a reboot
//
// TrendRing knows how to turn itself into bytes and back (snapshot/restore);
// this is the half that knows about LittleFS, so the data structure every
// reading passes through does not have to.
//
// Call order at boot matters and is not interchangeable:
//
//     kindleTrackTrends();   // decide which series exist
//     trendStoreLoad();      // give them back their history
//
// restore() merges by name into the series that are already tracked, so a
// load before the track()s would find nothing to fill.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_KINDLE_DASHBOARD

/// Read the snapshot back into trendRing. Quiet when there is no file — a
/// first boot is not an error — and loud when there is one it cannot use,
/// because a chart that silently starts empty is the symptom this whole
/// arrangement exists to remove.
void trendStoreLoad();

/// Save when an hour has completed since the last save. Call from loop();
/// it is a flag test until there is something to write, and about 24 writes
/// a day once there is.
void trendStoreTick();

/// Write now, whatever the flag says. For the paths that know the power is
/// about to go: a reboot from the UI, an OTA about to restart the device.
/// Returns false when the filesystem refused, which is worth logging but is
/// not worth delaying a restart for.
bool trendStoreSave();

#endif  // FEATURE_KINDLE_DASHBOARD
