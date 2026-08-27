#pragma once
// ============================================================================
// src/core/SdCompat.h — one place the SD card library enters the build.
//
// WHY THIS EXISTS
// ---------------
// `#include <SD.h>` costs about 34 KB of flash on the C3 — most of it FatFs
// (libfatfs.a: ff.c, diskio.c, ffsystem.c) plus the SD/SPI driver on top.
// Measured, not estimated: two full builds of xiao_esp32c3 differ by 34,576
// bytes of flashed image (firmware.bin 1,333,408 with, 1,298,832 without).
// That is real money on a 1472 KB app partition, and it is spent whether or
// not an SD card is ever fitted.
//
// So the include is behind FEATURE_SD_STORAGE (src/setup.h, ON by default so
// nothing changes for anyone using a card), and every `&SD` in the codebase
// goes through sdFs() instead. Turning the feature off then removes the
// library rather than merely skipping the code that calls it — a distinction
// that matters, because a reference anywhere keeps the archive linked.
//
// WHEN THE FEATURE IS OFF
// -----------------------
// sdFs() returns nullptr and sdAvailable is pinned false at init, so every
// call site's existing `&& sdAvailable` guard already short-circuits before
// the null can be dereferenced. Nothing needed a new branch; the guards were
// always there because an absent or unreadable card had to be handled anyway.
// ============================================================================

#include <FS.h>
#include "../setup.h"

#ifdef FEATURE_SD_STORAGE
#  include <SD.h>
#endif

/// The SD filesystem, or nullptr when SD support is compiled out.
///
/// Callers must keep testing `sdAvailable` as they always have — this returns
/// a non-null pointer whenever the library is present, including when no card
/// is inserted.
inline fs::FS* sdFs() {
#ifdef FEATURE_SD_STORAGE
    return &SD;
#else
    return nullptr;
#endif
}

/// True when the firmware was built with SD support at all. Distinct from
/// `sdAvailable`, which is about a card being present and mounted right now.
inline constexpr bool sdSupportCompiledIn() {
#ifdef FEATURE_SD_STORAGE
    return true;
#else
    return false;
#endif
}
