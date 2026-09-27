// ============================================================================
// node/src/FwFlash.h — write a node image into the ESP8266's update slot, and
// commit it only if it is one
//
// docs/NODE_OTA.md §3 and §5. Two ways in, one set of checks: the collector's
// offer (main.cpp downloads it) and the node page's upload (ConfigPortal.cpp
// receives it). Both stream through here, chunk by chunk, into the core's
// Update; every chunk also goes through nodefw::MarkerScan, and the first byte
// through nodefw::headOk. Nothing is committed until finish() has seen the
// whole image and found exactly one marker, of kind "esp8266".
//
// WHY THE CHECK HAS TO WAIT FOR THE LAST BYTE. The marker is a string
// constant; the linker puts it wherever .irom0.text puts it, a few hundred KB
// into the image. So the image is already in the update slot when the verdict
// is known, and refusing it means ending an update WITHOUT committing it —
// which the ESP8266 updater has no call for (there is no Update.abort() on
// this core). abort() below does it with a target MD5 that cannot match; the
// comment there cites the lines of Updater.cpp that make that safe.
// ============================================================================
#pragma once

#include <Arduino.h>

#include "src/nodecfg/FwImage.h"

class FwFlash {
public:
    FwFlash() { nodefw::scanBegin(_scan); }
    /// Never leaves an update half-open behind it: a download that returns
    /// early on any path is aborted, not committed.
    ~FwFlash() { abort(); }

    /// Start an update. `size` is the exact image size (a download: Update
    /// then refuses a short body itself) or 0 when it is not known up front
    /// (a multipart upload): the whole free sketch space is taken, as
    /// ESP8266HTTPUpdateServer does, and end() is told to take what came.
    /// `md5` (32 hex, or nullptr) is what the image must hash to.
    bool begin(uint32_t size, const char* md5);

    /// One chunk, in order. False once anything failed; error() says what.
    bool write(const uint8_t* p, size_t n);

    /// The whole image is in: check the marker, then commit (Update.end()).
    /// True = staged; the next restart runs it.
    bool finish();

    /// End an update without committing it. Safe at any point, and twice.
    void abort();

    bool active() const { return _active; }
    uint32_t written() const { return _written; }
    const char* version() const { return _scan.ver; }

    /// §5's error words: "not_node_image", "too_big", "write_failed", plus
    /// "md5_mismatch" and "short" for a download. nullptr = none.
    const char* error() const { return _err; }
    /// The updater's own words for a write_failed, for a person to read.
    const char* detail() const { return _detail; }

    /// Largest image this node can take now: what Update.begin() will allow
    /// (the space between this sketch and the filesystem, rounded down to a
    /// sector), never more than the kind's slot in FwImage.h.
    static uint32_t maxImage();

private:
    void fail(const char* err);

    nodefw::MarkerScan _scan;
    const char* _err     = nullptr;
    char        _detail[40] = "";
    uint32_t    _written = 0;
    bool        _exact   = false;
    bool        _active  = false;
    bool        _headSeen = false;
    bool        _headOk   = false;
};
