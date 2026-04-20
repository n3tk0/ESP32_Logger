#include "TimeModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"

namespace {

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char TIME_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"ntpServer\",\"type\":\"string\",\"max\":64,\"label\":\"NTP server\"},"
      "{\"id\":\"timezone\",\"type\":\"int\",\"min\":-12,\"max\":14,\"label\":\"Timezone (h from UTC)\"},"
      "{\"id\":\"dstOffsetHours\",\"type\":\"int\",\"min\":0,\"max\":2,\"label\":\"DST offset (h)\"}"
    "]}";

} // namespace

// ---------------------------------------------------------------------------
bool TimeModule::load(JsonObjectConst cfg) {
    NetworkConfig& n = config.network;
    const char* ntp = cfg["ntpServer"] | (const char*)nullptr;
    if (ntp) strlcpy(n.ntpServer, ntp, sizeof(n.ntpServer));
    n.timezone       = (int8_t)(cfg["timezone"]       | (int)n.timezone);
    n.dstOffsetHours = (int8_t)(cfg["dstOffsetHours"] | (int)n.dstOffsetHours);
    return true;
}

// ---------------------------------------------------------------------------
void TimeModule::save(JsonObject cfg) const {
    const NetworkConfig& n = config.network;
    cfg["ntpServer"]      = n.ntpServer;
    cfg["timezone"]       = (int)n.timezone;
    cfg["dstOffsetHours"] = (int)n.dstOffsetHours;
}

// ---------------------------------------------------------------------------
const char* TimeModule::schema() const {
    return TIME_SCHEMA;
}
