#include "WiFiModule.h"
#include "ModuleSchemas.h"      // this module's form, gzipped
#include "../core/Globals.h"
#include "../core/Config.h"
#include <WiFi.h>
#include "../utils/Ipv4Parse.h"
#include "../utils/JsonEnum.h"

namespace {

// Parse "a.b.c.d" → 4-byte array.  Returns false on malformed input (target untouched).
bool parseIPv4(const char* s, uint8_t out[4]) {
    if (!s) return true;  // absent field is not a validation error
    return ipv4Parse(s, out);   // not sscanf: see Ipv4Parse.h
}

} // namespace

void formatIPv4(const uint8_t in[4], char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", in[0], in[1], in[2], in[3]);
}

// ---------------------------------------------------------------------------
bool WiFiModule::load(JsonObjectConst cfg) {
    NetworkConfig& n = config.network;
    n.wifiMode       = (WiFiModeType)jsonEnumInt(cfg["wifiMode"], (int)n.wifiMode);
    bool ok = true;
    if ((int)n.wifiMode < 0 || (int)n.wifiMode > 1) { n.wifiMode = WIFIMODE_AP; ok = false; }
    n.useStaticIP    = cfg["useStaticIP"] | n.useStaticIP;

    const char* ssid = cfg["clientSSID"] | (const char*)nullptr;
    if (ssid) strlcpy(n.clientSSID, ssid, sizeof(n.clientSSID));
    // save() never sends the password back, so the form's field is always
    // empty and an untouched field posts "" — which used to wipe the stored
    // password on any WiFi save. Blank keeps it, as the field's help says.
    const char* pw = cfg["clientPassword"] | (const char*)nullptr;
    if (pw && pw[0]) strlcpy(n.clientPassword, pw, sizeof(n.clientPassword));

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
ModuleSchema WiFiModule::schema() const {
    return {MODULE_SCHEMA_WIFI_GZ, sizeof(MODULE_SCHEMA_WIFI_GZ)};
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
