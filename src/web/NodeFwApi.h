// ============================================================================
// src/web/NodeFwApi.h
//
// The collector's HTTP side of node firmware updates — docs/NODE_OTA.md §2.3.
//
//   GET  /api/nodes/fw             images and targets
//   POST /api/nodes/fw/upload      multipart, field "fw": a node image
//   POST /api/nodes/fw             {"action":"start"|"cancel"|"delete"|"min_mv",…}
//   GET  /api/nodes/fw/bin?kind=   the image itself, for a WiFi node (ingest token)
//
// Registered in ApiHandlers.cpp next to /api/nodes/config (so
// tools/check_api_docs.py sees them); the handlers are here. Everything here
// exists only with FEATURE_REMOTE_NODES.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_REMOTE_NODES

#include <Arduino.h>

class AsyncWebServerRequest;

void handleNodesFwGet(AsyncWebServerRequest* req);
void handleNodesFwBody(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total);
void handleNodesFwBin(AsyncWebServerRequest* req);

/// The two halves of the upload: onUpload streams the file to SD, onRequest
/// checks it and answers.
void handleNodesFwUploadDone(AsyncWebServerRequest* req);
void handleNodesFwUpload(AsyncWebServerRequest* req, const String& filename, size_t index,
                         uint8_t* data, size_t len, bool final);

#endif  // FEATURE_REMOTE_NODES
