#include "NodeSettings.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "node_config.h"

static const char* CFG_PATH = "/config.json";
static const char* TMP_PATH = "/config.tmp";

static void copyClamped(char* dst, size_t dstLen, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}

// Seed from the compile-time values so a first boot with build flags set
// behaves exactly as it did before the portal existed.
static void applyDefaults(NodeSettings& s) {
    copyClamped(s.ssid,      sizeof(s.ssid),      WIFI_SSID);
    copyClamped(s.pass,      sizeof(s.pass),      WIFI_PASS);
    copyClamped(s.host,      sizeof(s.host),      COLLECTOR_HOST);
    copyClamped(s.token,     sizeof(s.token),     INGEST_TOKEN);
    copyClamped(s.nodeId,    sizeof(s.nodeId),    NODE_ID);
    copyClamped(s.basicUser, sizeof(s.basicUser), COLLECTOR_BASIC_USER);
    copyClamped(s.basicPass, sizeof(s.basicPass), COLLECTOR_BASIC_PASS);
    s.port       = COLLECTOR_PORT;
    s.intervalMs = POST_INTERVAL_MS;
    s.altitudeM  = ALTITUDE_M;
    s.i2cSda     = I2C_SDA_PIN;
    s.i2cScl     = I2C_SCL_PIN;
    s.oneWirePin = ONEWIRE_PIN;
    s.pulsePin   = PULSE_PIN;
    s.sdsRx      = SDS011_RX_PIN;
    s.sdsTx      = SDS011_TX_PIN;

    // The placeholder SSID from node_config.h is not a network. Treating it
    // as one would make isComplete() true and send the node into a retry loop
    // against an AP that does not exist, instead of opening the portal.
    if (strcmp(s.ssid, "your-ssid") == 0) s.ssid[0] = '\0';
}

bool settingsLoad(NodeSettings& out) {
    applyDefaults(out);

    // A node fresh from the factory has no filesystem image; formatting on
    // first mount is the difference between "portal comes up" and "nothing
    // works and the serial log says FS error".
    if (!LittleFS.begin()) {
        Serial.println("[cfg] LittleFS mount failed, formatting");
        if (!LittleFS.format() || !LittleFS.begin()) {
            Serial.println("[cfg] format failed; running on defaults only");
            return false;
        }
    }

    File f = LittleFS.open(CFG_PATH, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[cfg] %s is corrupt (%s); using defaults\n",
                      CFG_PATH, err.c_str());
        return false;
    }

    // Each field falls back to what applyDefaults() already put there, so a
    // config written by an older firmware keeps working when a new setting
    // is added.
    copyClamped(out.ssid,      sizeof(out.ssid),      doc["ssid"]      | out.ssid);
    copyClamped(out.pass,      sizeof(out.pass),      doc["pass"]      | out.pass);
    copyClamped(out.host,      sizeof(out.host),      doc["host"]      | out.host);
    copyClamped(out.token,     sizeof(out.token),     doc["token"]     | out.token);
    copyClamped(out.nodeId,    sizeof(out.nodeId),    doc["nodeId"]    | out.nodeId);
    copyClamped(out.basicUser, sizeof(out.basicUser), doc["basicUser"] | out.basicUser);
    copyClamped(out.basicPass, sizeof(out.basicPass), doc["basicPass"] | out.basicPass);

    out.port       = doc["port"]       | out.port;
    out.intervalMs = doc["intervalMs"] | out.intervalMs;
    out.altitudeM  = doc["altitudeM"]  | out.altitudeM;
    out.i2cSda     = doc["i2cSda"]     | out.i2cSda;
    out.i2cScl     = doc["i2cScl"]     | out.i2cScl;
    out.oneWirePin = doc["oneWirePin"] | out.oneWirePin;
    out.pulsePin   = doc["pulsePin"]   | out.pulsePin;
    out.sdsRx      = doc["sdsRx"]      | out.sdsRx;
    out.sdsTx      = doc["sdsTx"]      | out.sdsTx;

    return true;
}

bool settingsSave(const NodeSettings& s) {
    JsonDocument doc;
    doc["ssid"]       = s.ssid;
    doc["pass"]       = s.pass;
    doc["host"]       = s.host;
    doc["port"]       = s.port;
    doc["token"]      = s.token;
    doc["nodeId"]     = s.nodeId;
    doc["basicUser"]  = s.basicUser;
    doc["basicPass"]  = s.basicPass;
    doc["intervalMs"] = s.intervalMs;
    doc["altitudeM"]  = s.altitudeM;
    doc["i2cSda"]     = s.i2cSda;
    doc["i2cScl"]     = s.i2cScl;
    doc["oneWirePin"] = s.oneWirePin;
    doc["pulsePin"]   = s.pulsePin;
    doc["sdsRx"]      = s.sdsRx;
    doc["sdsTx"]      = s.sdsTx;

    // Temp file + rename: a brownout partway through leaves the previous
    // config intact rather than a truncated one. Writing CFG_PATH directly
    // would make the save the most dangerous moment in the node's life.
    LittleFS.remove(TMP_PATH);
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) {
        Serial.println("[cfg] cannot open temp file");
        return false;
    }
    const size_t written = serializeJson(doc, f);
    f.close();

    if (written == 0) {
        Serial.println("[cfg] write produced no bytes");
        LittleFS.remove(TMP_PATH);
        return false;
    }
    if (!LittleFS.remove(CFG_PATH) && LittleFS.exists(CFG_PATH)) {
        Serial.println("[cfg] cannot replace existing config");
        LittleFS.remove(TMP_PATH);
        return false;
    }
    if (!LittleFS.rename(TMP_PATH, CFG_PATH)) {
        Serial.println("[cfg] rename failed");
        return false;
    }

    Serial.printf("[cfg] saved %u bytes\n", (unsigned)written);
    return true;
}

bool settingsErase() {
    return LittleFS.remove(CFG_PATH);
}
