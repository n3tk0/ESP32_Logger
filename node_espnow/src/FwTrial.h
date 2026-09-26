// ============================================================================
// node_espnow/src/FwTrial.h
//
// The trial a new firmware runs on, and the way back — docs/NODE_OTA.md §4.4.
// Pure decisions here (tests/host/test_espnow_node_fw.cpp); the NVS keys and
// the switch are in FwFetch.cpp.
//
// WHY THE NODE DOES THIS ITSELF
// -----------------------------
// A node on a fence post that boots an image which cannot reach the collector
// — a radio change that does not work, a crash three seconds in, a key it
// does not have — is a node somebody has to walk to with a cable. The C3 has
// two app slots, so the old firmware is still there; what is missing is
// something that decides to go back to it.
//
// The bootloader cannot be that something. Arduino-ESP32 2.0.17's prebuilt
// bootloader IS built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (its
// sdkconfig says so, and it carries write_otadata / set_actual_ota_seq), so
// an image it is switched to starts ESP_OTA_IMG_NEW → PENDING_VERIFY, and one
// that is still PENDING_VERIFY at the next boot is abandoned for the other
// slot. But initArduino() calls esp_ota_mark_app_valid_cancel_rollback()
// before setup() runs (verifyRollbackLater() is false unless a sketch
// overrides it), so that only ever catches an image that dies before
// setup(). (The collector's src/managers/OtaManager.cpp is written for a
// bootloader WITHOUT rollback, the older Arduino IDE ones; this node relies
// on neither, so it works on both.) The bootloader also falls back to the
// other slot when the chosen one fails its own checksum/SHA check at load.
// Both of those end with the OLD firmware running while the trial is armed:
// the "WentBack" case below. Everything else — a crash loop after setup(),
// an image that runs but never hears an ACK — is caught here, in the image
// on trial, by counting.
//
// WHAT IS COUNTED, AND WHY BOTH
// -----------------------------
//  * `boots` (NVS): every start of the image on trial. On a battery every
//    wake is a boot (deep sleep is a reset), so this counts wakes there, and
//    it counts crashes everywhere — a panic loop never gets as far as a wake.
//  * `misses` (RAM): unanswered wakes in a row. The only counter that moves
//    in MAINS mode, where the node boots once and then reports from loop()
//    for weeks — `boots` would sit at 1 while it never reached anybody.
//
// Either past its limit: set the previous slot as the boot partition,
// remember (img, attempt) as rolled back, restart. The first ACK — any ACK,
// it proves the radio, the key and the channel all work — confirms the image
// and clears the trial. Confirmation is one NVS write, once.
//
// NVS namespace "nodefw": prev, img, att, boots (the trial; prev = 0 is "no
// trial": address 0 is the bootloader, never an app slot), rb_img, rb_att
// (the last image rolled back from, kept until a different one is).
// ============================================================================
#pragma once

#include <stdint.h>

namespace enfwtrial {

/// More starts than this without an ACK: roll back. Three unanswered wakes on
/// a battery — the fourth start decides, before it spends a fourth wake.
static const uint8_t MAX_BOOTS  = 3;
/// Unanswered wakes in a row (mains mode's counter) before rolling back.
static const uint8_t MAX_MISSES = 3;

struct Trial {
    uint32_t prev;    ///< the partition the node came from, 0 = no trial
    uint32_t img;     ///< the image on trial
    uint8_t  att;     ///< its attempt
    uint8_t  boots;   ///< starts of that image so far
};

static inline bool armed(const Trial& t) { return t.prev != 0; }

static inline Trial arm(uint32_t prevAddr, uint32_t img, uint8_t att) {
    Trial t;
    t.prev  = prevAddr;
    t.img   = img;
    t.att   = att;
    t.boots = 0;
    return t;
}

enum class Boot : uint8_t {
    None,       ///< no trial
    Counted,    ///< on trial: store the new `boots`
    WentBack,   ///< the old firmware is running: the new one never started
    RollBack,   ///< too many starts: go back now
};

/// At boot, before anything that could crash. `runningAddr` is the running
/// partition's address. Updates `t.boots`.
static inline Boot onBoot(Trial& t, uint32_t runningAddr) {
    if (!armed(t)) return Boot::None;
    // Running from `prev` means the bootloader did not (or could not) start
    // the image on trial. Nothing to count and nothing to switch: record it.
    if (runningAddr == t.prev) return Boot::WentBack;
    if (t.boots < 0xFF) t.boots++;
    return t.boots > MAX_BOOTS ? Boot::RollBack : Boot::Counted;
}

enum class Wake : uint8_t {
    None,       ///< no trial
    Confirm,    ///< an ACK: the image works — clear the trial
    Counted,    ///< unanswered, still inside the limit
    RollBack,   ///< unanswered MAX_MISSES times in a row
};

/// After a wake's report. `misses` is the caller's run of unanswered wakes
/// (RAM: it only has to survive in mains mode, where there is no reset).
static inline Wake onWake(const Trial& t, bool acked, uint8_t& misses) {
    if (!armed(t)) return Wake::None;
    if (acked) {
        misses = 0;
        return Wake::Confirm;
    }
    if (misses < 0xFF) misses++;
    return misses >= MAX_MISSES ? Wake::RollBack : Wake::Counted;
}

}  // namespace enfwtrial

// ---------------------------------------------------------------------------
// The NVS and partition side — FwFetch.cpp (not compiled on the host)
// ---------------------------------------------------------------------------

/// First thing in setup(): count this start of an image on trial, notice the
/// bootloader went back, or roll back (does not return then: restarts).
/// Loads the rolled-back (img, attempt) for fwOnPending().
void fwTrialBoot();

/// After every report: confirm on an ACK, count a miss, or roll back.
void fwTrialAfterWake(bool acked);

/// Drop an armed trial without a verdict: a local upload replaced the image
/// (docs/NODE_OTA.md §5 — the person doing it is standing there), and the
/// slot the trial would go back to is the one that upload just overwrote.
void fwTrialClear();
