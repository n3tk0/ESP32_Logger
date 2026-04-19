#include "WiFiModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"

namespace {

// Parse "a.b.c.d" → 4-byte array.  Leaves target untouched on malformed input.
void parseIPv4(const char* s, uint8_t out[4]) {
    if (!s) return;
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
        (a | b | c | d) >= 0 && a <= 255 && b <= 255 && c <= 255 && d <= 255) {
        out[0] = (uint8_t)a; out[1] = (uint8_t)b;
        out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    }
}

void formatIPv4(const uint8_t in[4], char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", in[0], in[1], in[2], in[3]);
}

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char WIFI_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"wifiMode\",\"type\":\"enum\",\"label\":\"Mode\","
        "\"options\":[{\"v\":0,\"l\":\"Access Point\"},{\"v\":1,\"l\":\"Client\"}]},"
      "{\"id\":\"clientSSID\",\"type\":\"string\",\"max\":32,\"label\":\"SSID\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"clientPassword\",\"type\":\"password\",\"max\":64,\"label\":\"Password\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"useStaticIP\",\"type\":\"bool\",\"label\":\"Use static IP\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"staticIP\",\"type\":\"ipv4\",\"label\":\"IP\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"gateway\",\"type\":\"ipv4\",\"label\":\"Gateway\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"subnet\",\"type\":\"ipv4\",\"label\":\"Subnet\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"dns\",\"type\":\"ipv4\",\"label\":\"DNS\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"ntpServer\",\"type\":\"string\",\"max\":64,\"label\":\"NTP server\"},"
      "{\"id\":\"timezone\",\"type\":\"int\",\"min\":-12,\"max\":14,\"label\":\"Timezone\"},"
      "{\"id\":\"dstOffsetHours\",\"type\":\"int\",\"min\":0,\"max\":2,\"label\":\"DST offset\"}"
    "]}";

} // namespace

// ---------------------------------------------------------------------------
bool WiFiModule::load(JsonObjectConst cfg) {
    NetworkConfig& n = config.network;
    n.wifiMode       = (WiFiModeType)(cfg["wifiMode"] | (int)n.wifiMode);
    n.useStaticIP    = cfg["useStaticIP"] | n.useStaticIP;
    n.timezone       = (int8_t)(cfg["timezone"] | (int)n.timezone);
    n.dstOffsetHours = (int8_t)(cfg["dstOffsetHours"] | (int)n.dstOffsetHours);

    const char* ssid = cfg["clientSSID"] | (const char*)nullptr;
    if (ssid) strlcpy(n.clientSSID, ssid, sizeof(n.clientSSID));
    const char* pw = cfg["clientPassword"] | (const char*)nullptr;
    if (pw) strlcpy(n.clientPassword, pw, sizeof(n.clientPassword));
    const char* ntp = cfg["ntpServer"] | (const char*)nullptr;
    if (ntp) strlcpy(n.ntpServer, ntp, sizeof(n.ntpServer));

    parseIPv4(cfg["staticIP"] | (const char*)nullptr, n.staticIP);
    parseIPv4(cfg["gateway"]  | (const char*)nullptr, n.gateway);
    parseIPv4(cfg["subnet"]   | (const char*)nullptr, n.subnet);
    parseIPv4(cfg["dns"]      | (const char*)nullptr, n.dns);
    return true;
}

// ---------------------------------------------------------------------------
void WiFiModule::save(JsonObject cfg) const {
    const NetworkConfig& n = config.network;
    cfg["wifiMode"]       = (int)n.wifiMode;
    cfg["clientSSID"]     = n.clientSSID;
    // Intentionally omit clientPassword from the shadow file (phase 2) —
    // storing it in two places without encryption is worse than one.  The
    // real password continues to live in config.bin only.
    cfg["useStaticIP"]    = n.useStaticIP;
    cfg["ntpServer"]      = n.ntpServer;
    cfg["timezone"]       = (int)n.timezone;
    cfg["dstOffsetHours"] = (int)n.dstOffsetHours;

    char buf[16];
    formatIPv4(n.staticIP, buf, sizeof(buf)); cfg["staticIP"] = String(buf);
    formatIPv4(n.gateway,  buf, sizeof(buf)); cfg["gateway"]  = String(buf);
    formatIPv4(n.subnet,   buf, sizeof(buf)); cfg["subnet"]   = String(buf);
    formatIPv4(n.dns,      buf, sizeof(buf)); cfg["dns"]      = String(buf);
}

// ---------------------------------------------------------------------------
const char* WiFiModule::schema() const {
    return WIFI_SCHEMA;
}
