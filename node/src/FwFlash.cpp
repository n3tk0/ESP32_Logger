#include "FwFlash.h"

#include <Updater.h>

#include "FwOffer.h"
#include "NodeLog.h"

uint32_t FwFlash::maxImage() {
    // The ESP8266HTTPUpdateServer's formula: the free sketch space less one
    // sector, rounded down to a sector. On eagle.flash.4m1m that is ~2.4 MB,
    // far more than an image can be — the slot it runs from is 0xFF000, and
    // an image larger than that would be staged, copied over the running
    // sketch by eboot and never boot. FwImage.h's limit is the one that bites.
    const uint32_t free = ESP.getFreeSketchSpace();
    const uint32_t room = free > 0x1000 ? ((free - 0x1000) & 0xFFFFF000u) : 0;
    const uint32_t cap  = nodefw::maxSize(nodefw::KIND_ESP8266);
    return room < cap ? room : cap;
}

bool FwFlash::begin(uint32_t size, const char* md5) {
    abort();
    nodefw::scanBegin(_scan);
    _err = nullptr;
    _detail[0] = '\0';
    _written = 0;
    _headSeen = _headOk = false;

    const uint32_t room = maxImage();
    if (size > room || room == 0) {
        _err = "too_big";
        return false;
    }
    _exact = (size != 0);
    if (!Update.begin(_exact ? size : room, U_FLASH)) {
        _err = (Update.getError() == UPDATE_ERROR_SPACE) ? "too_big" : "write_failed";
        nodecfg::copyStr(_detail, sizeof(_detail), Update.getErrorString().c_str());
        return false;
    }
    _active = true;
    // After begin(), never before: begin() clears the target MD5
    // (Updater.cpp:108). setMD5() refuses anything but 32 characters by
    // returning false — and then end() would check nothing — so that is a
    // failure here, not a warning.
    if (md5 && !Update.setMD5(md5)) {
        fail("write_failed");
        nodecfg::copyStr(_detail, sizeof(_detail), "bad md5");
        return false;
    }
    return true;
}

bool FwFlash::write(const uint8_t* p, size_t n) {
    if (!_active) return false;
    if (n == 0) return true;
    if (!_headSeen) {
        // The first chunk holds the image header. A .bin that does not start
        // with 0xE9 is refused before a sector is erased for it. (Update would
        // also take 0x1F, a gzipped image; an image this node cannot scan
        // for its marker is not one it takes.)
        _headSeen = true;
        _headOk   = nodefw::headOk(nodefw::KIND_ESP8266, p, n);
        if (!_headOk) {
            fail("not_node_image");
            return false;
        }
    }
    nodefw::scanFeed(_scan, p, n);
    // Update.write() takes a non-const pointer and does not write through it:
    // it copies into its own sector buffer (Updater.cpp:449, 458).
    if (Update.write(const_cast<uint8_t*>(p), n) != n) {
        const uint8_t e = Update.getError();
        // write() refuses a chunk that would pass the size begin() was given
        // with UPDATE_ERROR_SPACE (Updater.cpp:440-443): for an upload that
        // is the free space, so the image is too big for this node.
        fail(e == UPDATE_ERROR_SPACE ? "too_big" : "write_failed");
        return false;
    }
    _written += n;
    return true;
}

bool FwFlash::finish() {
    if (!_active) {
        if (!_err) _err = "not_node_image";   // nothing arrived at all
        return false;
    }
    if (!NodeFw::imageIsOurs(_headSeen, _headOk, _scan)) {
        LOGF("[fw] refused: %s\n", nodefw::scanKind(_scan) == nodefw::KIND_NONE
                                       ? (_scan.conflict ? "two markers" : "no node marker")
                                       : "a marker of another kind");
        fail("not_node_image");
        return false;
    }
    // A download knows its size: end(false) refuses a short body itself
    // (Updater.cpp:226-231). An upload does not: end(true) takes what came.
    if (!Update.end(!_exact)) {
        const uint8_t e = Update.getError();
        if (e == UPDATE_ERROR_MD5) _err = "md5_mismatch";
        else if (e == UPDATE_ERROR_OK && _exact) _err = "short";
        else _err = "write_failed";
        nodecfg::copyStr(_detail, sizeof(_detail), Update.getErrorString().c_str());
        // An MD5 mismatch returns WITHOUT resetting the updater
        // (Updater.cpp:328-333), which would leave it "running" and refuse
        // the next begin() (Updater.cpp:69-74). abort() finishes the job.
        abort();
        return false;
    }
    _active = false;
    LOGF("[fw] %u bytes verified; \"%s\" staged\n", (unsigned)_written, _scan.ver);
    return true;
}

// THE MD5 TRICK (docs/NODE_OTA.md §3 step 3). This core's UpdaterClass has no
// abort(); end() is the only way out of an update, and end() on a complete
// image COMMITS it. So the target MD5 is replaced with all zeros, which no
// real image hashes to, and end() is left to fail. In framework-arduinoespressif8266 3.30102.0,
// cores/esp8266/Updater.cpp:
//
//   * 203-209  setMD5() only stores the string; it can be called at any time.
//   * 226-231  an update that already has an error, or is short and not told
//              evenIfRemaining, is _reset() and end() returns false.
//   * 328-333  with no signature verifier installed (this node installs
//              none), a stored target MD5 that differs from the computed one
//              sets UPDATE_ERROR_MD5 and returns false...
//   * 344-350  ...before the eboot command — the ONLY thing that makes the
//              next boot copy the staged image over the sketch — is written.
//              The running firmware is untouched; the staged bytes are just
//              unused flash.
//   * 330-332  the MD5 failure path does not _reset(), so the updater still
//              says isRunning() and begin() would refuse the next update
//              (69-74). The second end() below takes the 226-231 path
//              (hasError()) and resets it.
//
// end(true) rather than end(): a partial update then also reaches the MD5
// check instead of being reset early — either way nothing is committed, and
// one sequence covers every state the update can be in.
void FwFlash::abort() {
    if (Update.isRunning()) {
        Update.setMD5("00000000000000000000000000000000");
        Update.end(true);
        if (Update.isRunning()) Update.end();
    }
    _active = false;
}

void FwFlash::fail(const char* err) {
    if (!_err) _err = err;
    abort();
}
