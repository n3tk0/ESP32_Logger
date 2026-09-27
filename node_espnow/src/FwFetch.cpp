// ============================================================================
// node_espnow/src/FwFetch.cpp — the flash side of a firmware update
//
// docs/NODE_OTA.md §4.3 (download, verify, switch) and §4.4 (the trial). The
// decisions are FwFetch.h's and FwTrial.h's; this file only carries them out
// against the radio, the partitions and NVS.
//
// WHY THE RAW PARTITION API AND NOT esp_ota_begin()/Update
// --------------------------------------------------------
// esp_ota_begin() erases the whole slot up front — 1280 KB, several seconds —
// and its handle does not survive a deep sleep. A download here spans wakes,
// so the node erases each 4 KB sector as the write enters it and keeps its
// own "how far" in RTC memory; esp_ota_set_boot_partition() at the end does
// the one check that matters (the image's own SHA-256) either way.
//
// WHAT THE NODE RELIES ON FROM IDF 4.4 (Arduino-ESP32 2.0.17)
// ------------------------------------------------------------
//  * esp_ota_set_boot_partition() runs esp_image_verify() on the slot — the
//    header, every segment's checksum and the appended SHA-256 — and refuses
//    it with ESP_ERR_OTA_VALIDATE_FAILED before touching otadata. So a
//    truncated or corrupted download can never become the boot image.
//  * It then writes the next otadata sequence (state NEW with this SDK's
//    CONFIG_APP_ROLLBACK_ENABLE). The bootloader boots it, and — see
//    FwTrial.h — falls back to the other slot on its own only when the image
//    fails to load. Everything after that is the trial's job.
//  * Going back is the same call on the previous slot: that image passed the
//    same check when it was installed, and is still intact because nothing
//    writes a running slot.
// ============================================================================
#include "FwFetch.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "CfgFetch.h"
#include "FwTrial.h"
#include "Link.h"
#include "node_config.h"
#include "src/nodecfg/FwImage.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// RTC_NOINIT, reset by fwColdBoot() under main.cpp's rules — so a new image
// (whose first boot is always "cold": s_rtcImage changes) starts with none of
// this, and a download interrupted by a panic starts again from byte 0.
RTC_NOINIT_ATTR static enfw::Progress s_prog;
RTC_NOINIT_ATTR static enfw::Failed   s_failed;
RTC_NOINIT_ATTR static encfg::Backoff s_backoff;

/// The trial as NVS holds it, and the image last rolled back from. Read once
/// per boot by fwTrialBoot().
static enfwtrial::Trial s_trial;
static uint32_t         s_rbImg = 0;
static uint8_t          s_rbAtt = 0;
static uint8_t          s_trialMisses = 0;   ///< mains mode's counter (FwTrial.h)

static const char* const NS = "nodefw";

void fwColdBoot() {
    enfw::forget(s_prog);
    memset(&s_failed, 0, sizeof(s_failed));
    s_backoff = encfg::Backoff{0, 0};
}

// ---------------------------------------------------------------------------
// The trial — NVS
// ---------------------------------------------------------------------------

static void loadTrial() {
    memset(&s_trial, 0, sizeof(s_trial));
    Preferences p;
    // Read-only open of a namespace that was never written fails, which is
    // the ordinary case: no trial, nothing rolled back.
    if (!p.begin(NS, true)) return;
    s_trial.prev  = p.getUInt("prev", 0);
    s_trial.img   = p.getUInt("img", 0);
    s_trial.att   = p.getUChar("att", 0);
    s_trial.boots = p.getUChar("boots", 0);
    s_rbImg       = p.getUInt("rb_img", 0);
    s_rbAtt       = p.getUChar("rb_att", 0);
    p.end();
}

static bool saveTrial(const enfwtrial::Trial& t) {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putUInt("prev", t.prev) == sizeof(uint32_t);
    ok = p.putUInt("img", t.img) == sizeof(uint32_t) && ok;
    ok = p.putUChar("att", t.att) == 1 && ok;
    ok = p.putUChar("boots", t.boots) == 1 && ok;
    p.end();
    if (ok) s_trial = t;
    return ok;
}

static void clearTrial() {
    Preferences p;
    if (p.begin(NS, false)) {
        p.remove("prev");
        p.remove("img");
        p.remove("att");
        p.remove("boots");
        p.end();
    }
    memset(&s_trial, 0, sizeof(s_trial));
    s_trialMisses = 0;
}

static void recordRolledBack(uint32_t img, uint8_t att) {
    Preferences p;
    if (p.begin(NS, false)) {
        p.putUInt("rb_img", img);
        p.putUChar("rb_att", att);
        p.end();
    }
    s_rbImg = img;
    s_rbAtt = att;
}

static const esp_partition_t* appAt(uint32_t addr) {
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    const esp_partition_t* found = nullptr;
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p->address == addr) {
            found = p;
            break;
        }
    }
    esp_partition_iterator_release(it);   // NULL-safe
    return found;
}

/// Back to the slot the trial came from, and restart. Returns only when the
/// switch failed — then there is nothing to go back to, and the image on
/// trial keeps running without one.
static void rollBack() {
    Serial.printf("[fw] image %08lx never reached the collector — going back\n",
                  (unsigned long)s_trial.img);
    const esp_partition_t* prev = appAt(s_trial.prev);
    const esp_err_t e = prev ? esp_ota_set_boot_partition(prev) : ESP_ERR_NOT_FOUND;
    // Recorded either way: the collector is told ROLLED_BACK for this exact
    // (image, attempt) only, and a node still running it answers RUNNING
    // first (FwFetch.h), which is the truth.
    recordRolledBack(s_trial.img, s_trial.att);
    clearTrial();
    if (e != ESP_OK) {
        Serial.printf("[fw] cannot go back (%s) — staying on this image\n", esp_err_to_name(e));
        return;
    }
    Serial.flush();
    esp_restart();
}

void fwTrialBoot() {
    loadTrial();
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run) return;
    switch (enfwtrial::onBoot(s_trial, run->address)) {
        case enfwtrial::Boot::None:
            return;
        case enfwtrial::Boot::Counted:
            saveTrial(s_trial);
            Serial.printf("[fw] on trial: image %08lx, start %u of %u\n",
                          (unsigned long)s_trial.img, (unsigned)s_trial.boots,
                          (unsigned)enfwtrial::MAX_BOOTS);
            return;
        case enfwtrial::Boot::WentBack:
            Serial.printf("[fw] image %08lx did not start — the bootloader went back\n",
                          (unsigned long)s_trial.img);
            recordRolledBack(s_trial.img, s_trial.att);
            clearTrial();
            return;
        case enfwtrial::Boot::RollBack:
            rollBack();
            return;
    }
}

void fwTrialAfterWake(bool acked) {
    switch (enfwtrial::onWake(s_trial, acked, s_trialMisses)) {
        case enfwtrial::Wake::None:
        case enfwtrial::Wake::Counted:
            return;
        case enfwtrial::Wake::Confirm:
            Serial.printf("[fw] image %08lx confirmed\n", (unsigned long)s_trial.img);
            clearTrial();
            return;
        case enfwtrial::Wake::RollBack:
            rollBack();
            return;
    }
}

void fwTrialClear() {
    if (enfwtrial::armed(s_trial)) clearTrial();
}

// ---------------------------------------------------------------------------
// Writing and verifying
// ---------------------------------------------------------------------------

static bool writeSlice(const esp_partition_t* part, const FwChunkMsg& m) {
    uint32_t first = 0;
    const uint32_t n = enfw::sectorsEntered(m.offset, m.len, first);
    for (uint32_t i = 0; i < n; i++)
        if (esp_partition_erase_range(part, first + i * enfw::SECTOR, enfw::SECTOR) != ESP_OK)
            return false;
    return esp_partition_write(part, m.offset, m.data, m.len) == ESP_OK;
}

/// Everything §4.3 step 6 asks before the switch, then the switch and the
/// trial. EN_FW_ST_STAGED, or the status to report.
static uint8_t stage(const esp_partition_t* part, uint32_t imgId, uint32_t size, uint8_t att) {
    // The marker and the head, over what is actually in flash — not over the
    // slices as they arrived, which a resumed download never saw all of.
    static uint8_t buf[1024];
    uint8_t head[nodefw::HEAD_NEED];
    nodefw::MarkerScan scan;
    nodefw::scanBegin(scan);
    if (size < nodefw::HEAD_NEED) return EN_FW_ST_BAD_IMAGE;
    for (uint32_t off = 0; off < size;) {
        const uint32_t n = size - off < sizeof(buf) ? size - off : (uint32_t)sizeof(buf);
        if (esp_partition_read(part, off, buf, n) != ESP_OK) return EN_FW_ST_FLASH_ERROR;
        if (off == 0) memcpy(head, buf, sizeof(head));   // n >= HEAD_NEED here
        nodefw::scanFeed(scan, buf, n);
        off += n;
    }
    if (!nodefw::headOk(nodefw::KIND_ESPNOW_C3, head, sizeof(head)) ||
        nodefw::scanKind(scan) != nodefw::KIND_ESPNOW_C3 || nodefw::c3ImageId(head) != imgId) {
        Serial.printf("[fw] not an espnow-c3 image with id %08lx\n", (unsigned long)imgId);
        return EN_FW_ST_BAD_IMAGE;
    }
    // The same id as IDF reads it — the number the new image will report as
    // running, so a mismatch here is a "done" that could never happen.
    esp_app_desc_t d;
    uint32_t id = 0;
    if (esp_ota_get_partition_description(part, &d) != ESP_OK) return EN_FW_ST_BAD_IMAGE;
    memcpy(&id, d.app_elf_sha256, sizeof(id));
    if (id != imgId) return EN_FW_ST_BAD_IMAGE;

    // The image's own SHA-256, checked by the switch itself.
    const esp_err_t e = esp_ota_set_boot_partition(part);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) return EN_FW_ST_BAD_IMAGE;
    if (e != ESP_OK) return EN_FW_ST_FLASH_ERROR;

    // Armed AFTER the switch succeeded and BEFORE the restart. Unable to arm
    // it, the node does not switch at all: an image without a way back is
    // exactly what the trial exists to prevent.
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (!run || !saveTrial(enfwtrial::arm(run->address, imgId, att))) {
        if (run) esp_ota_set_boot_partition(run);
        return EN_FW_ST_FLASH_ERROR;
    }
    return EN_FW_ST_STAGED;
}

static void sendDone(const NodeLink& link, uint32_t imgId, uint8_t status, uint8_t att,
                     uint16_t value) {
    FwDoneMsg m;
    espnowFillFwDone(m, link.nodeId, imgId, status, att, value);
    const bool ok = linkSendFrame(link, &m, sizeof(m));
    Serial.printf("[fw] FW_DONE %08lx status %u -> %s\n", (unsigned long)imgId,
                  (unsigned)status, ok ? "sent" : "not delivered");
}

// ---------------------------------------------------------------------------
// One wake's download
// ---------------------------------------------------------------------------

void fwOnPending(const NodeLink& link, uint32_t runningId, uint16_t vbatMv, bool mains,
                 uint16_t replyWindowMs) {
    if (!encfg::due(s_backoff)) return;

    // Only one slot on this table: nothing to write into, ever.
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        Serial.println("[fw] no update partition");
        encfg::failed(s_backoff);
        return;
    }

    static enfw::Fetch f;
    static FwChunkMsg  rep;          // 221 bytes: off the loop task's stack
    const uint32_t t0 = millis();
    enfw::begin(f, link.nodeId, runningId, s_rbImg, s_rbAtt, vbatMv, part->address, part->size,
                t0, mains ? NODE_FW_BUDGET_MAINS_MS : NODE_FW_BUDGET_MS, NODE_FW_MAX_MISSES);
    const uint32_t from = s_prog.written;

    FwGetMsg req;
    for (;;) {
        if (!enfw::wantRequest(f, millis(), req)) {
            // Out of budget or of replies. Progress made = the next wake
            // carries on; none = the collector is struggling, back off.
            Serial.printf("[fw] %lu/%lu bytes, stopping after %lu ms\n",
                          (unsigned long)s_prog.written, (unsigned long)s_prog.size,
                          (unsigned long)(millis() - t0));
            if (f.gained) encfg::succeeded(s_backoff);
            else          encfg::failed(s_backoff);
            return;
        }
        uint32_t w = enfw::remainingMs(f, millis());
        if (w > replyWindowMs) w = replyWindowMs;
        const bool got = linkExchangeFw(link, req, (uint16_t)w, rep);
        enfw::Step step = enfw::onReply(f, s_prog, s_failed, got ? &rep : nullptr);

        if (step == enfw::Step::Write) step = enfw::wrote(f, s_prog, writeSlice(part, *f.slice));

        switch (step) {
            case enfw::Step::Request:
            case enfw::Step::Write:           // wrote() never returns it
                continue;
            case enfw::Step::Busy:
                Serial.println("[fw] collector busy with another node");
                encfg::failed(s_backoff);
                return;
            case enfw::Step::Nothing:
                Serial.println("[fw] nothing for us any more");
                encfg::succeeded(s_backoff);
                return;
            case enfw::Step::Complete: {
                Serial.printf("[fw] %08lx: %lu bytes (%lu this wake) in %lu ms — verifying\n",
                              (unsigned long)f.imgId, (unsigned long)s_prog.size,
                              (unsigned long)(s_prog.size - from),
                              (unsigned long)(millis() - t0));
                const uint8_t st = stage(part, s_prog.imgId, s_prog.size, s_prog.attempt);
                if (st == EN_FW_ST_STAGED) {
                    sendDone(link, s_prog.imgId, EN_FW_ST_STAGED, s_prog.attempt, 0);
                    enfw::forget(s_prog);
                    Serial.println("[fw] staged — restarting into it");
                    Serial.flush();
                    esp_restart();
                }
                f.imgId   = s_prog.imgId;
                f.attempt = s_prog.attempt;
                f.status  = st;
                f.value   = 0;
            }
                [[fallthrough]];   // report the failure
            case enfw::Step::Report:
                sendDone(link, f.imgId, f.status, f.attempt, f.value);
                if (f.status == EN_FW_ST_BAD_IMAGE || f.status == EN_FW_ST_FLASH_ERROR) {
                    s_failed = enfw::Failed{f.imgId, f.attempt, f.status};
                    enfw::forget(s_prog);
                }
                if (f.status == EN_FW_ST_RUNNING) encfg::succeeded(s_backoff);
                else                              encfg::failed(s_backoff);
                return;
        }
    }
}
