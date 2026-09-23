// ============================================================================
// src/nodes/NodeCfgStore.h
//
// The collector's copy of every node's configuration — docs/NODE_CONFIG.md §2.
//
// One file per node on LittleFS:
//
//   /nodes/w_<name>.json   a WiFi node, keyed by the id it posts as
//   /nodes/e_<id>.json     an ESP-NOW node, keyed by its radio id
//
//   { "desired": {…§1 WITH secrets…}, "applied_rev": 3,
//     "reported": {…§1 without secrets…}, "status": "applied|pending|rejected",
//     "error": {"field","reason"}, "seen": <epoch>,
//     "ho_rev": 7, "sec_known": 3, "sec_dirty": 0 }
//
// The last three are the collector's own bookkeeping (§4's handover rev and
// the secret bits explained in NodeCfgRules.h); unknown keys to anybody else.
//
// A small index of every file is kept in RAM — revs, status, the last error,
// and for an ESP-NOW node the probe names DATA2 needs — so a status list or a
// radio frame never has to touch the filesystem. Files are read only when a
// config itself is needed, and written only when something changed: a WiFi
// node posting its unchanged cfg_rev every minute costs no flash.
//
// A file exists only once the node has reported its config, or been adopted
// on first contact. Nothing is invented for a node that never reported: a
// config made up from defaults and pushed to it would wipe its sensor list.
//
// THREADS. The async web task (ingest, the Nodes page API) and loop() (the
// ESP-NOW tick, the handover) both call in; one mutex serialises every call,
// and every file access inside also takes fsMutex (Pillar 1.3). The ESP-NOW
// receive callback never calls in at all — EspNowIngest mirrors what it needs.
//
// Compiled only with FEATURE_REMOTE_NODES.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <ArduinoJson.h>

#include "NodeCfgRules.h"

#ifndef NODECFG_MAX_NODES
#  define NODECFG_MAX_NODES 16   ///< config files held: ESPNOW_MAX_NODES (8) + 8 WiFi nodes
#endif
/// Most nodes a handover walks: every file, plus nodes in the status lists
/// without one.
#define NODECFG_LIST_MAX (NODECFG_MAX_NODES + 24)

/// Load the index from /nodes. Call once from setup, before the web server
/// starts; every other call is a no-op until it has run.
void nodeCfgBegin();

/// Bumped on every change — EspNowIngest re-mirrors when it moves.
uint32_t nodeCfgGeneration();

struct NodeCfgSummary {
    uint16_t       rev;       ///< desired
    uint16_t       applied;
    uint16_t       hoRev;     ///< rev that carried the handover's next network, 0 = none
    uint8_t        status;    ///< ncr::ST_*
    nodecfg::Issue err;       ///< when rejected
};

/// Add `"cfg": {"key","rev","applied_rev","status","error"?}` to a node of a
/// status list (§7) — nothing when the node has no config file.
void nodeCfgPutSummary(JsonObject node, bool espnow, const char* name, uint8_t id);

// ── WiFi node, over /api/ingest (§3) ────────────────────────────────────────

/// Read cfg_rev / cfg / cfg_error from an ingest body and act on them; put
/// "cfg" into `reply` when the node is due one. A body without cfg_rev (a node
/// that predates §3) is ignored entirely.
void nodeCfgIngest(const char* node, JsonObjectConst body, JsonObject reply);

// ── ESP-NOW node (§5), all called from espnowIngestTick() ───────────────────

struct NodeCfgRadio {
    bool     have;        ///< a config file exists
    uint16_t rev;         ///< desired
    uint16_t applied;
    uint8_t  status;
};
/// False only when the store was busy — the caller should ask again later.
bool nodeCfgRadioState(uint8_t id, NodeCfgRadio& out);

/// A completed CFG_REPORT. `told` is the plan the receive callback worked
/// out from its mirror, and for a local report already answered with
/// (`told.applied` is what the node now believes it runs; any other report
/// was answered with its own rev, which is what is recorded); the store keeps to
/// it, moving only the desired rev on if a web edit landed in between.
/// `label` / `intervalS` are the node table's; on first contact they win
/// (ncr::adoptTableIdentity), after a local edit the table follows the node —
/// the store updates it.
void nodeCfgEspnowReport(uint8_t id, const char* json, size_t len,
                         const char* label, uint16_t intervalS, ncr::ReportPlan told);

/// A CFG_ACK. An ok one for the desired rev also makes `desired` the
/// reported config: the node does not report again after applying (§5).
void nodeCfgEspnowAck(uint8_t id, uint16_t rev, bool ok, const char* field, const char* reason);

/// The desired config as the radio carries it (§5: compact, no net, no
/// secrets, no key). Returns its length, 0 when there is nothing to send or it
/// does not fit `cap` / EN_CFG_MAX_TOTAL.
size_t nodeCfgRadioDoc(uint8_t id, char* buf, size_t cap, uint16_t& rev);

/// One DATA2 sample as named readings (§5): probes named through the config
/// the node last reported, or probe_temp[_N] before it has. Returns how many.
int nodeCfgData2Named(uint8_t id, const Data2Sample& s, ncr::NamedValue* out, int max);

/// Forget an ESP-NOW node's file (the node was removed from the table).
void nodeCfgForget(bool espnow, const char* name, uint8_t id);

// ── The Nodes page API (§7) ─────────────────────────────────────────────────

/// GET /api/nodes/config. A key with no file (a node that has not reported
/// yet) is answered with nulls, §7. Returns the HTTP status.
int nodeCfgApiGet(const char* key, JsonDocument& out);

/// POST /api/nodes/config. Returns the HTTP status.
int nodeCfgApiPost(JsonObjectConst body, JsonDocument& out);

// ── Handover (§4) ───────────────────────────────────────────────────────────

/// Whether a handover is running; if so, and when given, the network it
/// hands out and the millis() it started at.
bool nodeCfgHandover(char ssid[nodecfg::SSID_CAP], uint32_t* startMs);

/// §4 step 3: sort every node that could be asked to follow into
/// ready / pending / offline. Those are the nodes with a config file and
/// those in either status list without one (they cannot follow, and say so
/// by never becoming ready). Fills `out` with the three key lists unless it
/// is null; returns how many are pending.
int nodeCfgHandoverSort(JsonObject out);

/// Start (or restart) a handover: every node with a config is handed the
/// next network in a new rev, and `form` is kept for the switch.
bool nodeCfgHandoverStart(const char* ssid, const char* pass, JsonVariantConst form);

/// Take the next network back from every node (another rev) and stop.
bool nodeCfgHandoverCancel();

/// End the handover for the switch: copies out what it was started with and
/// forgets it. `form` is null in `out` when none was given.
bool nodeCfgHandoverFinish(JsonDocument& out);

#endif  // FEATURE_REMOTE_NODES
