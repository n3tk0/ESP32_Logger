// ============================================================================
// node_espnow/src/CfgApply.h
//
// Turning a config document the collector sent (reassembled CFG slices) into
// the config the node will run — or into the reason it will not. Pure apart
// from ArduinoJson, so tests/host runs the exact code the node does.
//
// The rules, all from docs/NODE_CONFIG.md:
//   * decode on top of the RUNNING config (a partial document is an edit);
//   * `rev` comes from the frame header, not the body, and a config from the
//     collector is never `local`;
//   * identity (transport, hw, fw) and the key are the node's own — never
//     read from the radio (§1 decoding rules, principle 6);
//   * the shared validator decides; a refusal leaves the running config
//     untouched and names the field (principle 3).
// ============================================================================
#pragma once

#include <stdint.h>

#include "src/nodecfg/NodeConfigJson.h"

namespace encfg {

/// Decode + validate. True: `next` is the config to save and run. False:
/// `why` holds the {field, reason} for the CFG_ACK, and `next` means nothing.
static inline bool acceptFromCollector(const nodecfg::NodeConfig& running, const char* doc,
                                       uint16_t len, uint16_t rev, nodecfg::NodeConfig& next,
                                       nodecfg::Issue& why) {
    using namespace nodecfg;
    why.field[0]  = '\0';
    why.reason[0] = '\0';
    next = running;

    JsonDocument jd;
    if (!doc || deserializeJson(jd, doc, len) != DeserializationError::Ok) {
        copyStr(why.reason, sizeof(why.reason), "the config is not valid JSON");
        return false;
    }
    if (!decodeConfig(jd.as<JsonVariantConst>(), next, NCJ_DEC_REV, &why)) return false;

    next.rev       = rev;
    next.local     = false;
    next.transport = running.transport;
    next.hw        = running.hw;
    copyStr(next.fw, sizeof(next.fw), running.fw);
    copyStr(next.lmk, sizeof(next.lmk), running.lmk);

    Validation v;
    if (!validate(next, v)) {
        why = v.error;
        return false;
    }
    return true;
}

}  // namespace encfg
