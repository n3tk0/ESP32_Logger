#include "TimeModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"
#include "../managers/RtcManager.h"   // applyTimeZone
#include "../utils/PosixTz.h"
#include <stdlib.h>                  // atoi

namespace {

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char TIME_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"ntpServer\",\"type\":\"string\",\"max\":64,\"label\":\"NTP server\",\"group\":\"NTP\","
        "\"help\":\"Hostname queried at boot and on a manual sync (e.g. pool.ntp.org).\"},"
      "{\"id\":\"timezone\",\"type\":\"int\",\"min\":-12,\"max\":14,\"label\":\"Timezone\",\"unit\":\"h\","
        "\"help\":\"Hours from UTC. Timestamps are stored in UTC and displayed in this zone.\"},"
      "{\"id\":\"dstRule\",\"type\":\"enum\",\"label\":\"Daylight saving\","
        "\"options\":[{\"v\":0,\"l\":\"Automatic (EU)\"},{\"v\":1,\"l\":\"Automatic (US)\"},"
                     "{\"v\":2,\"l\":\"Off\"},{\"v\":3,\"l\":\"Manual (always on)\"}]},"
      "{\"id\":\"dstOffsetHours\",\"type\":\"int\",\"min\":1,\"max\":2,\"label\":\"DST offset\",\"unit\":\"h\","
        "\"help\":\"Hours added all year under the manual rule.\",\"showIf\":{\"dstRule\":3}}"
    "]}";

} // namespace

// ---------------------------------------------------------------------------
bool TimeModule::load(JsonObjectConst cfg) {
    NetworkConfig& n = config.network;
    const char* ntp = cfg["ntpServer"] | (const char*)nullptr;
    if (ntp) strlcpy(n.ntpServer, ntp, sizeof(n.ntpServer));
    n.timezone       = (int8_t)(cfg["timezone"]       | (int)n.timezone);
    if (n.timezone < -12 || n.timezone > 14) n.timezone = 0;
    n.dstOffsetHours = (int8_t)(cfg["dstOffsetHours"] | (int)n.dstOffsetHours);
    if (n.dstOffsetHours < 0 || n.dstOffsetHours > 2) n.dstOffsetHours = 0;
    // The module form posts an enum as the <select>'s string value ("2"),
    // which `| int` would silently ignore.
    JsonVariantConst rule = cfg["dstRule"];
    if (rule.is<int>())              n.dstRule = dstRuleClamp((uint8_t)rule.as<int>());
    else if (rule.is<const char*>()) n.dstRule = dstRuleClamp((uint8_t)atoi(rule.as<const char*>()));
    applyTimeZone();
    return true;
}

// ---------------------------------------------------------------------------
bool TimeModule::save(JsonObject cfg) const {
    const NetworkConfig& n = config.network;
    cfg["ntpServer"]      = n.ntpServer;
    cfg["timezone"]       = (int)n.timezone;
    cfg["dstOffsetHours"] = (int)n.dstOffsetHours;
    cfg["dstRule"]        = (int)n.dstRule;
    return true;
}

// ---------------------------------------------------------------------------
// R20: hot-start hook — queues a non-blocking NTP sync. The actual sync
// runs from loop() (driven by g_pendingNtpSync) so a /api/modules/time/
// restart call returns immediately instead of holding the AsyncTCP
// worker for the round trip + DNS lookup + UDP exchange.
bool TimeModule::start() {
    g_pendingNtpSync = 1;
    return true;
}

// ---------------------------------------------------------------------------
const char* TimeModule::schema() const {
    return TIME_SCHEMA;
}

// ---------------------------------------------------------------------------
// Live status chip — reuses the same NTP-sync globals as /api/time_sync_status
// (g_pendingNtpSync: 0 idle/1 requested/2 running; g_lastNtpSyncResult: 0
// unknown/1 ok/-1 fail).  Reading volatiles only — safe on the AsyncTCP worker.
void TimeModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;                       // UI shows "disabled"
    if (g_pendingNtpSync != 0)        { out["text"] = "syncing\xE2\x80\xA6"; out["tone"] = "dim";  return; }
    if (g_lastNtpSyncResult == 1)     { out["text"] = "synced";             out["tone"] = "ok";   return; }
    if (g_lastNtpSyncResult == -1)    { out["text"] = "sync failed";        out["tone"] = "warn"; return; }
    out["text"] = "not synced yet"; out["tone"] = "dim";
}
