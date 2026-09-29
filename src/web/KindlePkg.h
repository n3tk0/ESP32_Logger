// ============================================================================
// src/web/KindlePkg.h
//
// Kindle dashboard updates through the collector — docs/KINDLE_UPDATE.md.
//
// The same shape as the node firmware updates (docs/NODE_OTA.md): the
// collector keeps ONE package on its SD card, the person chooses to offer it,
// and every reader PULLS it — a reader learns about it from the /kindle/data
// payload it fetches anyway, downloads /kindle/pkg.tar, checks it, installs
// it over its own folder and restarts into it. The collector never connects
// to a reader.
//
// On the SD card (sdAvailable), under /kindlepkg:
//
//   esp32dash.tar   the package, as tools/mk_kindle_package.sh builds it
//   pkg.json        {"ver","size","md5","uploaded","offer"}
//   upload.tmp      an upload in flight
//
// The readers that have fetched a page are remembered in RAM only — by
// address, with the version they reported and how their update went — so the
// page can say which one runs what. A reboot forgets them until their next
// fetch, a few minutes later.
//
// THREADS. Everything here runs on the async web task — the API, the upload,
// /kindle/data and the download — except kindlePkgBegin(), which runs from
// setup before the server does. So the state needs no lock of its own; every
// SD access holds fsMutex (Pillar 1.3).
//
// Compiled only with FEATURE_KINDLE_DASHBOARD.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>

#include "KindlePkgTar.h"

class AsyncWebServerRequest;

#define KINDLEPKG_DIR "/kindlepkg"
#define KINDLEPKG_TMP KINDLEPKG_DIR "/upload.tmp"

/// Load the package's description from the card. Call once, after the SD
/// mount and before the web server starts.
void kindlePkgBegin();

/// What /kindle/data tells a reader (§3): filled, and true, when there is a
/// package on offer and the reader reports running something else.
struct KindlePkgOffer {
    char     ver[kpkg::VER_CAP];
    char     md5[33];
    uint32_t size;
};

/// Called by /kindle/data for every fetch: records the reader (its address,
/// `?pkg=` and `?pkgerr=`) and says whether to offer it the package. A
/// reader that sends no `pkg` runs a script from before this existed and is
/// never offered anything — it would not know what to do with it.
bool kindlePkgOffer(AsyncWebServerRequest* req, KindlePkgOffer& out);

// ── HTTP (registered in ApiHandlers.cpp and KindleDashboard.cpp) ────────────

/// GET  /api/kindle/pkg          the package and the readers
void handleKindlePkgGet(AsyncWebServerRequest* req);
/// POST /api/kindle/pkg          form: action=offer|stop|delete
void handleKindlePkgPost(AsyncWebServerRequest* req);
/// POST /api/kindle/pkg/upload   multipart, field "pkg"
void handleKindlePkgUpload(AsyncWebServerRequest* req, const String& filename,
                           size_t index, uint8_t* data, size_t len, bool final);
void handleKindlePkgUploadDone(AsyncWebServerRequest* req);
/// GET  /kindle/pkg.tar          the package, for a reader
void handleKindlePkgTar(AsyncWebServerRequest* req);

#endif  // FEATURE_KINDLE_DASHBOARD
