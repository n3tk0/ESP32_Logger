// ============================================================================
// src/nodes/NodeFwStore.h
//
// The collector's node firmware images and who should run them —
// docs/NODE_OTA.md §2.
//
// On the SD card (sdAvailable), under /nodefw:
//
//   esp8266.bin / espnow-c3.bin     the image, at most one per kind
//   esp8266.json / espnow-c3.json   {"kind","ver","size","md5","sha256",
//                                    "uploaded"[, "img_id","min_mv"]}
//   rollout.json                    per kind: the attempt and every target
//   upload.tmp                      an upload in flight (NodeFwApi.cpp)
//
// All of it is mirrored in RAM at boot, so the ingest handler and the ESP-NOW
// tick answer from memory and touch the card only for image bytes
// (nodeFwRead). rollout.json is written when a status changes — never for
// progress, which lives in RAM only.
//
// THREADS. The async web task (the API, ingest, the image download) and
// loop() (the ESP-NOW tick) call in; one mutex serialises them, and every SD
// access inside also holds fsMutex (Pillar 1.3), taken after it. The ESP-NOW
// receive callback never calls in: EspNowIngest mirrors what it needs, as it
// does for the config (NodeCfgStore).
//
// Compiled only with FEATURE_REMOTE_NODES.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <ArduinoJson.h>

#include "NodeFwRules.h"

#ifndef NODEFW_MAX_TARGETS
#  define NODEFW_MAX_TARGETS 24   ///< ESPNOW_MAX_NODES (8) + 16 WiFi nodes
#endif

#define NODEFW_DIR "/nodefw"
#define NODEFW_TMP NODEFW_DIR "/upload.tmp"

/// Load the images and the rollout from the card. Call once from setup,
/// after the SD mount; every other call is a no-op until it has run.
void nodeFwBegin();

/// Bumped on every image or status change — EspNowIngest re-mirrors on it.
uint32_t nodeFwGeneration();

// ── Image bytes (the download and the radio) ────────────────────────────────

/// Identity of the image of `kind` at this moment. `serial` changes whenever
/// the image is replaced or deleted, so a reader that started on one image
/// never gets bytes of the next. `size` 0 = there is none. False only when
/// the store was busy: "busy" must never read as "no image", which the radio
/// would pass on as "forget your download".
struct NodeFwImage {
    uint32_t serial;
    uint32_t size;
    uint32_t imgId;     ///< espnow-c3
    uint16_t minMv;     ///< espnow-c3
    uint8_t  attempt;
    char     md5[33];
};
bool nodeFwImage(nodefw::Kind kind, NodeFwImage& out);

/// Read `len` bytes at `off` of image `serial` of `kind`. Returns the bytes
/// read, 0 when that image is gone (or the read failed), -1 when the card is
/// busy right now (try again).
int nodeFwRead(nodefw::Kind kind, uint32_t serial, uint32_t off, uint8_t* buf, size_t len);

// ── WiFi node (§3) ──────────────────────────────────────────────────────────

/// Read fw_md5 / fw_error from an ingest body; put "fw" into `reply` when the
/// node is offered the image.
void nodeFwIngest(const char* node, JsonObjectConst body, JsonObject reply);

// ── ESP-NOW node (§4), from espnowIngestTick() ──────────────────────────────

/// Target status of ESP-NOW node `id` (nfr::ST_*; ST_NONE = not a target, or
/// no image), NODEFW_ST_BUSY when the store was busy — ask again later.
#define NODEFW_ST_BUSY 0xFF
uint8_t nodeFwRadioStatus(uint8_t id);

/// The node asked for image `imgId` from `off` (progress, pending → sending).
void nodeFwEspnowGet(uint8_t id, uint32_t imgId, uint32_t off);

/// A FW_DONE.
void nodeFwEspnowDone(uint8_t id, uint32_t imgId, uint8_t status, uint8_t attempt, uint16_t value);

// ── The Nodes page (§2.3) ───────────────────────────────────────────────────

/// GET /api/nodes/fw.
void nodeFwApiGet(JsonDocument& out);

/// POST /api/nodes/fw. Returns the HTTP status.
int nodeFwApiPost(JsonObjectConst body, JsonDocument& out);

/// What an upload measured, for nodeFwCommit().
struct NodeFwMeta {
    nodefw::Kind kind;
    uint32_t     size;
    uint32_t     imgId;
    char         ver[nodefw::VER_CAP];
    char         md5[33];
    char         sha256[65];
};

/// Make the checked NODEFW_TMP the image of `m.kind`: every target of that
/// kind back to pending with a new attempt. False when it could not be
/// moved into place.
bool nodeFwCommit(const NodeFwMeta& m);

#endif  // FEATURE_REMOTE_NODES
