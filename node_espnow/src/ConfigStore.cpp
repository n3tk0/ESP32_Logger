#include "ConfigStore.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <string.h>

#include "node_config.h"
#include "src/nodecfg/NodeConfigJson.h"

using namespace nodecfg;

static const char* const NS_CFG  = "cfg";
static const char* const KEY_DOC = "doc";
static const char* const KEY_LMK = "lmk";

/// The placeholder in node_config.h and in the collector's EspNowIngest.cpp.
static const char* const PLACEHOLDER_LMK = "change-this-key!";

void cfgStoreDefaults(NodeConfig& c) {
    c = configDefaults(Transport::EspNow, Hw::Esp32c3);
    copyStr(c.fw, sizeof(c.fw), NODE_FW_VERSION);

    // A name that tells two nodes apart on the collector's list before anyone
    // has named them: "node-" and the last two bytes of the MAC, the same
    // suffix the setup AP uses.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[NODE_NAME_MAX + 1] = "node-";
    static const char kHex[] = "0123456789ABCDEF";
    char* p = name + 5;
    *p++ = kHex[mac[4] >> 4];
    *p++ = kHex[mac[4] & 15];
    *p++ = kHex[mac[5] >> 4];
    *p++ = kHex[mac[5] & 15];
    *p   = '\0';
    copyStr(c.name, sizeof(c.name), name);

    // interval_s: the value a node built before the config document existed
    // was last told by its collector ("iv" in the link namespace), else the
    // compiled one. Reading "iv" is the whole migration: the first save of a
    // document supersedes it.
    uint16_t iv = NODE_INTERVAL_S;
    {
        Preferences old;
        if (old.begin("espnow-node", true)) {
            iv = old.getUShort("iv", NODE_INTERVAL_S);
            old.end();
        }
    }
    c.interval_s = iv >= INTERVAL_MIN_S ? iv : (uint16_t)NODE_INTERVAL_S;

    c.sleep   = true;
    c.board   = 0;                      // Seeed XIAO ESP32-C3
    c.i2c.sda = NODE_I2C_SDA;
    c.i2c.scl = NODE_I2C_SCL;

    SensorCfg bmx = sensorDefaults(SensorType::Bmx280, Hw::Esp32c3);
    bmx.addr = NODE_BMX_ADDR;
    addSensor(c, bmx);

    c.batt.pin     = NODE_BATT_PIN;
    c.batt.divider = NODE_BATT_DIVIDER;
    c.batt.trim    = NODE_BATT_TRIM;

    c.link.ack_window_ms = NODE_ACK_WINDOW_MS;
    c.link.rescan_fails  = NODE_RESCAN_FAILS;
    c.link.rescan_min_s  = NODE_RESCAN_MIN_INTERVAL_S;
}

void cfgStoreLoad(NodeConfig& out) {
    cfgStoreDefaults(out);

    Validation v;
    if (!validate(out, v)) {
        // Only a -D override can do this (a pin on the flash bus, say). Run it
        // anyway — refusing to start would leave a node nobody can reach —
        // but say so where a bench console will show it.
        Serial.printf("[cfg] compiled defaults do not validate: %s: %s\n",
                      v.error.field, v.error.reason);
    }

    Preferences prefs;
    if (!prefs.begin(NS_CFG, true)) return;   // namespace absent: first boot

    // getString() into a buffer, not the String overload: no heap, and a
    // stored value longer than the buffer (it cannot be; saves are refused
    // past 1 KB) returns 0 rather than a truncated document.
    char buf[EN_CFG_MAX_TOTAL + 1];
    if (prefs.isKey(KEY_DOC) && prefs.getString(KEY_DOC, buf, sizeof(buf)) > 1) {
        JsonDocument doc;
        Issue err;
        static NodeConfig tmp;           // ~1 KB: off the loop task's stack
        tmp = out;
        if (deserializeJson(doc, buf) != DeserializationError::Ok) {
            Serial.println("[cfg] stored config is not JSON — using defaults");
        } else if (!decodeConfig(doc.as<JsonVariantConst>(), tmp, NCJ_DEC_REV, &err)) {
            Serial.printf("[cfg] stored config refused: %s: %s\n", err.field, err.reason);
        } else if (!validate(tmp, v)) {
            Serial.printf("[cfg] stored config invalid: %s: %s\n",
                          v.error.field, v.error.reason);
        } else {
            out = tmp;
        }
    }

    char key[LMK_LEN + 1] = "";
    // getString() returns the length WITH the terminator (nvs_get_str's
    // count), so check the string rather than the return value.
    if (prefs.isKey(KEY_LMK) && prefs.getString(KEY_LMK, key, sizeof(key)) &&
        strlen(key) == LMK_LEN)
        copyStr(out.lmk, sizeof(out.lmk), key);
    prefs.end();

    // Identity is the firmware's to state, never the document's.
    out.transport = Transport::EspNow;
    out.hw        = Hw::Esp32c3;
    copyStr(out.fw, sizeof(out.fw), NODE_FW_VERSION);
}

bool cfgStoreSave(const NodeConfig& c) {
    char buf[EN_CFG_MAX_TOTAL + 1];
    const size_t n = encodeConfigTo(c, buf, sizeof(buf), 0);
    if (!n) {
        Serial.println("[cfg] config does not fit 1 KB — not saved");
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(NS_CFG, false)) return false;
    const bool ok = prefs.putString(KEY_DOC, buf) == n;
    prefs.end();
    return ok;
}

bool cfgStoreSaveKey(const char* lmk) {
    Preferences prefs;
    if (!prefs.begin(NS_CFG, false)) return false;
    bool ok;
    if (lmk && strlen(lmk) == LMK_LEN) {
        ok = prefs.putString(KEY_LMK, lmk) == LMK_LEN;
    } else {
        ok = !prefs.isKey(KEY_LMK) || prefs.remove(KEY_LMK);
    }
    prefs.end();
    return ok;
}

const char* cfgActiveKey(const NodeConfig& c) {
    return strlen(c.lmk) == LMK_LEN ? c.lmk : ESPNOW_LMK;
}

bool cfgKeyIsPlaceholder(const NodeConfig& c) {
    return memcmp(cfgActiveKey(c), PLACEHOLDER_LMK, LMK_LEN) == 0;
}
