// ============================================================================
// src/web/NodeCfgApi.h
//
// The collector's HTTP side of node configuration — docs/NODE_CONFIG.md §7 —
// and the two things that have to run on loop() for it: the UDP discovery
// responder (§3.1) and the handover's automatic switch (§4).
//
//   GET  /api/nodes/config?key=w:balcony|e:3
//   POST /api/nodes/config      {"key","config":{…partial §1…}}
//   GET  /api/nodes/handover
//   POST /api/nodes/handover    {"action":"start","ssid","pass","form"?}
//                               {"action":"switch"} / {"action":"cancel"}
//
// The routes are registered in ApiHandlers.cpp with the rest of the API (so
// tools/check_api_docs.py sees them); the handlers are here. Everything here
// exists only with FEATURE_REMOTE_NODES.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

class AsyncWebServerRequest;

void handleNodesConfigGet(AsyncWebServerRequest* req);
void handleNodesHandoverGet(AsyncWebServerRequest* req);

/// Body callbacks for the two POSTs: they accumulate a body larger than one
/// TCP segment (a full config is ~1.5 KB) and answer once it is complete.
void handleNodesConfigBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                           size_t index, size_t total);
void handleNodesHandoverBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                             size_t index, size_t total);

/// Shared with NodeFwApi.cpp: a JSON body accumulated across segments
/// (NODES_MAX_BODY), admitted by requireMutatingAuth() once whole, parsed,
/// and handed to `fn` — or answered 400 when it is not a JSON object.
typedef void (*JsonBodyFn)(AsyncWebServerRequest* req, JsonDocument& body);
void nodesApiBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index,
                  size_t total, JsonBodyFn fn);
/// Send `doc` as the answer, with `code`.
void nodesApiSend(AsyncWebServerRequest* req, int code, const JsonDocument& doc);

/// From loop(): answer discovery queries, and switch networks once every
/// node that can follow is ready.
void nodeCfgApiTick();

#endif  // FEATURE_REMOTE_NODES
