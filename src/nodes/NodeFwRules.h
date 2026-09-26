// ============================================================================
// src/nodes/NodeFwRules.h
//
// The collector's decisions about node firmware updates, as pure functions.
//
// docs/NODE_OTA.md is the contract. Like NodeCfgRules.h, the collector side
// is mostly plumbing — an SD card, a radio, HTTP — around a few decisions
// that are easy to get subtly wrong and impossible to see on a bench:
//
//   * what a target's status becomes on each report (§2.2);
//   * whether a WiFi node's POST gets the "fw" offer (§3);
//   * whether an ESP-NOW ACK carries EN_ACK_FW_PENDING (§4.1);
//   * what a FW_DONE means (§4.3);
//   * which 4 KB window of the image a FW_GET is answered from, and which
//     one the loop should read next (§4.2).
//
// They live here with no Arduino, no filesystem and no radio, so that
// tests/host/test_node_fw_rules.cpp can walk every branch.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../nodecfg/FwImage.h"
#include "../espnow/EspNowProto.h"

namespace nfr {

// ---------------------------------------------------------------------------
// Target status — §2.2
// ---------------------------------------------------------------------------

/// ST_NONE is "not a target" and is never written to rollout.json.
enum : uint8_t {
    ST_NONE = 0, ST_PENDING, ST_SENDING, ST_STAGED, ST_DEFERRED, ST_DONE, ST_FAILED, ST_COUNT
};

static const char* const ST_NAMES[ST_COUNT] = {
    "", "pending", "sending", "staged", "deferred", "done", "failed"
};

static inline const char* statusName(uint8_t s) { return s < ST_COUNT ? ST_NAMES[s] : ""; }

static inline uint8_t statusParse(const char* s) {
    for (uint8_t i = 1; s && i < ST_COUNT; i++)
        if (strcmp(s, ST_NAMES[i]) == 0) return i;
    return ST_NONE;
}

/// Still to be offered the image: failed and done are terminal (until the
/// user retries), and nothing else is a target at all.
static inline bool isOpen(uint8_t st) { return st != ST_NONE && st != ST_DONE && st != ST_FAILED; }

/// §3: a WiFi node is offered the image while pending or sending.
static inline bool wifiOffered(uint8_t st) { return st == ST_PENDING || st == ST_SENDING; }

/// §4.1: the ACK flag. Staged and deferred stay flagged — a staged node that
/// restarted into the image reports RUNNING on its next FW_GET(ANY), and a
/// deferred one checks its battery again.
static inline bool ackFlag(uint8_t st) { return isOpen(st); }

/// §4.5: the per-kind counter. It travels as a byte and 0 is "none" on the
/// node, so it wraps 255 → 1.
static inline uint8_t nextAttempt(uint8_t a) { return a >= 255 ? 1 : (uint8_t)(a + 1); }

// ---------------------------------------------------------------------------
// Keys — the Nodes page's "w:<name>" / "e:<id>" (full syntax: ncr::parseKey)
// ---------------------------------------------------------------------------

static inline nodefw::Kind kindOfKey(const char* key) {
    if (!key || !key[0] || key[1] != ':' || !key[2]) return nodefw::KIND_NONE;
    if (key[0] == 'w') return nodefw::KIND_ESP8266;
    if (key[0] == 'e') return nodefw::KIND_ESPNOW_C3;
    return nodefw::KIND_NONE;
}

// ---------------------------------------------------------------------------
// WiFi node — §3
// ---------------------------------------------------------------------------

enum : uint8_t { W_NONE = 0, W_OFFER, W_DONE, W_FAILED };

/// What one ingest POST does to target status `st`. `nodeMd5` is the POST's
/// fw_md5 ("" when absent — a node that predates OTA is never offered what it
/// cannot take), `imgMd5` the image's. `errMd5`/`errAttempt` are the POST's
/// fw_error (errMd5 null when there is none): it counts only for this image
/// and this attempt — a failure of an earlier one says nothing about a Retry.
///
/// Running the image wins over everything (§0.5), a matching error comes
/// next, and only then an offer: a node that failed is not offered again in
/// the reply that carried the failure.
static inline uint8_t wifiIngest(uint8_t st, const char* nodeMd5, const char* imgMd5,
                                 const char* errMd5, long errAttempt, uint8_t attempt) {
    if (st == ST_NONE || !imgMd5 || !imgMd5[0] || !nodeMd5 || !nodeMd5[0]) return W_NONE;
    if (strcmp(nodeMd5, imgMd5) == 0) return st == ST_DONE ? W_NONE : W_DONE;
    if (errMd5 && strcmp(errMd5, imgMd5) == 0 && errAttempt == (long)attempt && wifiOffered(st))
        return W_FAILED;
    return wifiOffered(st) ? W_OFFER : W_NONE;
}

// ---------------------------------------------------------------------------
// ESP-NOW node — §4
// ---------------------------------------------------------------------------

/// The status a FW_DONE moves target `st` to, or ST_NONE to ignore it. RUNNING
/// counts from any state and any attempt (done means running); the rest only
/// for the current attempt and an open target — a late STAGED from a
/// cancelled or superseded rollout must not resurrect it. `imgOk` = the
/// frame names the image the collector holds.
static inline uint8_t afterDone(uint8_t st, bool imgOk, uint8_t status, uint8_t frameAttempt,
                                uint8_t attempt) {
    if (st == ST_NONE || !imgOk) return ST_NONE;
    if (status == EN_FW_ST_RUNNING) return st == ST_DONE ? ST_NONE : ST_DONE;
    if (!isOpen(st) || frameAttempt != attempt) return ST_NONE;
    switch (status) {
        case EN_FW_ST_STAGED:      return ST_STAGED;
        case EN_FW_ST_LOW_BATTERY: return ST_DEFERRED;
        case EN_FW_ST_BAD_IMAGE:
        case EN_FW_ST_FLASH_ERROR:
        case EN_FW_ST_ROLLED_BACK: return ST_FAILED;
        default:                   return ST_NONE;
    }
}

/// The `err` a FW_DONE leaves behind ("" for none). LOW_BATTERY names the
/// voltage, "battery 3.41 V".
static inline void doneError(char* out, size_t cap, uint8_t status, uint16_t mv) {
    const char* s = "";
    switch (status) {
        case EN_FW_ST_BAD_IMAGE:   s = "bad image"; break;
        case EN_FW_ST_FLASH_ERROR: s = "flash error"; break;
        case EN_FW_ST_ROLLED_BACK: s = "rolled back"; break;
        case EN_FW_ST_LOW_BATTERY: {
            const unsigned cv = (mv + 5u) / 10u;   // centivolts, rounded
            snprintf(out, cap, "battery %u.%02u V", cv / 100u, cv % 100u);
            return;
        }
        default: break;
    }
    snprintf(out, cap, "%s", s);
}

/// A FW_GET naming the image (not EN_FW_ANY) is a download under way: a
/// pending or deferred target becomes sending. False = no status change.
static inline bool getMeansSending(uint8_t st) { return st == ST_PENDING || st == ST_DEFERRED; }

/// Percent of `size` the node has asked up to. 0..100.
static inline uint8_t pct(uint32_t offset, uint32_t size) {
    if (!size) return 0;
    if (offset >= size) return 100;
    return (uint8_t)((uint64_t)offset * 100u / size);
}

// ---------------------------------------------------------------------------
// Serving windows — §4.2
// ---------------------------------------------------------------------------
// Two 4 KB windows of the image in RAM. The receive callback answers from a
// window that holds the requested offset; the loop fills windows from SD.
// Everything in Serve is touched under one spinlock by both — the callback
// through lookup(), the loop through take()/loaded() — so a window is never
// read while it is being written: take() empties it first.
//
// A slice ends at its window's end, so it may be shorter than 200 bytes; the
// node asks next at offset + len. That is what lets a window start at any
// offset (the one a miss asked for) instead of a 4 KB boundary.

static const uint32_t WIN       = 4096;
static const uint32_t IDLE_MS   = 3000;   ///< a transfer silent this long is dropped

struct Win {
    uint32_t base;
    uint32_t len;     ///< 0 = empty or being loaded
};

struct Serve {
    Win      w[2];
    int8_t   loading;   ///< window the loop is filling, -1 none
    uint32_t loadOff;
    bool     req;       ///< a load is asked for and not yet taken
    uint8_t  reqIdx;
    uint32_t reqOff;
};

static inline void serveReset(Serve& s) {
    memset(&s, 0, sizeof(s));
    s.loading = -1;
}

static inline bool winHas(const Win& w, uint32_t off) {
    return w.len && off >= w.base && off - w.base < w.len;
}

/// Is `off` covered by a load asked for or under way?
static inline bool comingSoon(const Serve& s, uint32_t off) {
    if (s.req && off >= s.reqOff && off - s.reqOff < WIN) return true;
    return s.loading >= 0 && off >= s.loadOff && off - s.loadOff < WIN;
}

static inline void askLoad(Serve& s, uint8_t idx, uint32_t off) {
    s.req    = true;
    s.reqIdx = idx;
    s.reqOff = off;
}

/// A FW_GET for `off` of an image of `size`. Returns the window to answer
/// from and sets `n` to the slice length, or -1 (no answer now; a load is
/// asked for when none that covers `off` is already coming). A hit past the
/// middle of its window asks for the bytes after it in the other one.
static inline int lookup(Serve& s, uint32_t off, uint32_t size, uint8_t& n) {
    n = 0;
    if (off >= size) return -1;
    for (int i = 0; i < 2; i++) {
        const Win& w = s.w[i];
        if (!winHas(w, off)) continue;
        uint32_t k = w.base + w.len - off;
        if (k > EN_FW_CHUNK_MAX) k = EN_FW_CHUNK_MAX;
        n = (uint8_t)k;
        const uint32_t next = w.base + w.len;
        const int o = 1 - i;
        if (off - w.base >= WIN / 2 && next < size && !winHas(s.w[o], next) &&
            !comingSoon(s, next) && s.loading != o)
            askLoad(s, (uint8_t)o, next);
        return i;
    }
    if (comingSoon(s, off)) return -1;
    // The window to overwrite: one that is empty, else the one further
    // behind — the other may be the read-ahead the node is about to reach.
    uint8_t v;
    if (s.loading >= 0)       v = (uint8_t)(1 - s.loading);
    else if (!s.w[0].len)     v = 0;
    else if (!s.w[1].len)     v = 1;
    else                      v = s.w[0].base <= s.w[1].base ? 0 : 1;
    if (s.loading == (int8_t)v) return -1;
    askLoad(s, v, off);
    return -1;
}

/// Loop: take the asked-for load, if any, and empty that window so the
/// callback stops answering from it. Returns false when there is none or a
/// load is already under way.
static inline bool take(Serve& s, uint8_t& idx, uint32_t& off) {
    if (!s.req || s.loading >= 0) return false;
    s.req = false;
    idx = s.reqIdx;
    off = s.reqOff;
    s.loading = (int8_t)idx;
    s.loadOff = off;
    s.w[idx].len = 0;
    return true;
}

/// Loop: `len` bytes are now in window `idx` (0 on a read error: it stays
/// empty and the next miss asks again).
static inline void loaded(Serve& s, uint8_t idx, uint32_t len) {
    s.w[idx].base = s.loadOff;
    s.w[idx].len  = len;
    s.loading = -1;
}

/// How many bytes a window load at `off` reads.
static inline uint32_t loadLen(uint32_t off, uint32_t size) {
    return off >= size ? 0 : (size - off < WIN ? size - off : WIN);
}

/// One transfer at a time: is node `id` turned away because `owner` has
/// asked within IDLE_MS?
static inline bool busyFor(uint8_t owner, uint32_t idleMs, uint8_t id) {
    return owner != 0 && owner != id && idleMs < IDLE_MS;
}

}  // namespace nfr
