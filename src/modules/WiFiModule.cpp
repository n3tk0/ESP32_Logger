#include "WiFiModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"
#include <WiFi.h>

namespace {

// Parse "a.b.c.d" → 4-byte array.  Returns false on malformed input (target untouched).
bool parseIPv4(const char* s, uint8_t out[4]) {
    if (!s) return true;  // absent field is not a validation error
    int a, b, c, d;
    if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b;
    out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

void formatIPv4(const uint8_t in[4], char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", in[0], in[1], in[2], in[3]);
}

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char WIFI_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"wifiMode\",\"type\":\"enum\",\"label\":\"Mode\",\"group\":\"Connection\","
        "\"help\":\"Access Point hosts its own network; Client joins an existing one.\","
        "\"options\":[{\"v\":0,\"l\":\"Access Point\"},{\"v\":1,\"l\":\"Client\"}]},"
      "{\"id\":\"clientSSID\",\"type\":\"string\",\"max\":32,\"label\":\"SSID\",\"group\":\"Connection\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"clientPassword\",\"type\":\"password\",\"max\":64,\"label\":\"Password\",\"group\":\"Connection\","
        "\"help\":\"Leave blank to keep the currently stored password.\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"useStaticIP\",\"type\":\"bool\",\"label\":\"Use static IP\",\"group\":\"Static IP\","
        "\"help\":\"Off = DHCP. On = use the fixed addresses below.\","
        "\"showIf\":{\"wifiMode\":1}},"
      "{\"id\":\"staticIP\",\"type\":\"ipv4\",\"label\":\"IP\",\"group\":\"Static IP\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"gateway\",\"type\":\"ipv4\",\"label\":\"Gateway\",\"group\":\"Static IP\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"subnet\",\"type\":\"ipv4\",\"label\":\"Subnet\",\"group\":\"Static IP\",\"showIf\":\"useStaticIP\"},"
      "{\"id\":\"dns\",\"type\":\"ipv4\",\"label\":\"DNS\",\"group\":\"Static IP\",\"showIf\":\"useStaticIP\"}"
    "]}";

} // namespace

// ---------------------------------------------------------------------------
bool WiFiModule::load(JsonObjectConst cfg) {
    NetworkConfig& n = config.network;
    n.wifiMode       = (WiFiModeType)(cfg["wifiMode"] | (int)n.wifiMode);
    bool ok = true;
    if ((int)n.wifiMode < 0 || (int)n.wifiMode > 1) { n.wifiMode = WIFIMODE_AP; ok = false; }
    n.useStaticIP    = cfg["useStaticIP"] | n.useStaticIP;

    const char* ssid = cfg["clientSSID"] | (const char*)nullptr;
    if (ssid) strlcpy(n.clientSSID, ssid, sizeof(n.clientSSID));
    const char* pw = cfg["clientPassword"] | (const char*)nullptr;
    if (pw) strlcpy(n.clientPassword, pw, sizeof(n.clientPassword));

    ok &= parseIPv4(cfg["staticIP"] | (const char*)nullptr, n.staticIP);
    ok &= parseIPv4(cfg["gateway"]  | (const char*)nullptr, n.gateway);
    ok &= parseIPv4(cfg["subnet"]   | (const char*)nullptr, n.subnet);
    ok &= parseIPv4(cfg["dns"]      | (const char*)nullptr, n.dns);
    return ok;
}

// ---------------------------------------------------------------------------
bool WiFiModule::save(JsonObject cfg) const {
    const NetworkConfig& n = config.network;
    cfg["wifiMode"]       = (int)n.wifiMode;
    cfg["clientSSID"]     = n.clientSSID;
    // Intentionally omit clientPassword from the shadow file (phase 2) —
    // storing it in two places without encryption is worse than one.  The
    // real password continues to live in config.bin only.
    cfg["useStaticIP"]    = n.useStaticIP;

    char buf[16];
    formatIPv4(n.staticIP, buf, sizeof(buf)); cfg["staticIP"] = String(buf);
    formatIPv4(n.gateway,  buf, sizeof(buf)); cfg["gateway"]  = String(buf);
    formatIPv4(n.subnet,   buf, sizeof(buf)); cfg["subnet"]   = String(buf);
    formatIPv4(n.dns,      buf, sizeof(buf)); cfg["dns"]      = String(buf);
    return true;
}

// ---------------------------------------------------------------------------
const char* WiFiModule::schema() const {
    return WIFI_SCHEMA;
}

// ---------------------------------------------------------------------------
// Live status chip — reports the actual radio state (SSID · IP · RSSI in
// client mode, AP IP in AP mode).  WiFi.* getters are cheap, non-blocking.
void WiFiModule::statusJson(JsonObject out) const {
    if (!isEnabled()) return;                       // UI shows "disabled"
    const NetworkConfig& n = config.network;
    if (n.wifiMode == WIFIMODE_CLIENT) {
        if (WiFi.status() == WL_CONNECTED) {
            String t = WiFi.SSID();
            if (t.length()) t += " \xC2\xB7 ";
            t += WiFi.localIP().toString();
            long rssi = WiFi.RSSI();
            if (rssi != 0) { t += " \xC2\xB7 "; t += String(rssi); t += " dBm"; }
            out["text"] = t;  out["tone"] = "ok";
        } else {
            out["text"] = "not connected";  out["tone"] = "warn";
        }
    } else {  // Access-Point mode
        out["text"] = String("AP \xC2\xB7 ") + WiFi.softAPIP().toString();
        out["tone"] = "ok";
    }
}
