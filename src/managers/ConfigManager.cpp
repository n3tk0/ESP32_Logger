#include "ConfigManager.h"
#include "../core/Globals.h"
#include <LittleFS.h>
#include "esp_mac.h"


// ============================================================================
// INTERNAL: apply safe defaults to any zero/empty fields
// Called after every load path so partially-migrated or corrupt-but-recoverable
// configs always surface sane values to the web UI.
// ============================================================================
static void applyDefaults() {
    // ── Flow meter ────────────────────────────────────────────────────────────
    if (config.flowMeter.pulsesPerLiter            <= 0) config.flowMeter.pulsesPerLiter            = 450.0f;
    if (config.flowMeter.calibrationMultiplier     <= 0) config.flowMeter.calibrationMultiplier     = 1.0f;
    if (config.flowMeter.monitoringWindowSecs      <= 0) config.flowMeter.monitoringWindowSecs      = 3;
    if (config.flowMeter.firstLoopMonitoringWindowSecs <= 0) config.flowMeter.firstLoopMonitoringWindowSecs = 6;
    if (config.flowMeter.blinkDuration             <= 0) config.flowMeter.blinkDuration             = 250;

    // ── Hardware ──────────────────────────────────────────────────────────────
    if (config.hardware.cpuFreqMHz  <= 0) config.hardware.cpuFreqMHz  = 80;
    if (config.hardware.debounceMs  == 0) config.hardware.debounceMs  = 100;

    // ── Datalog ───────────────────────────────────────────────────────────────
    if (config.datalog.pfToFfThreshold <= 0) config.datalog.pfToFfThreshold = 4.5f;
    if (config.datalog.ffToPfThreshold <= 0) config.datalog.ffToPfThreshold = 3.7f;
    if (config.datalog.maxSizeKB       == 0) config.datalog.maxSizeKB       = 1024;
    if (!strlen(config.datalog.prefix))       strcpy(config.datalog.prefix,      DEFAULT_DATALOG_PREFIX);
    if (!strlen(config.datalog.currentFile))  strcpy(config.datalog.currentFile, "/datalog.txt");

    // ── Network ───────────────────────────────────────────────────────────────
    if (!strlen(config.network.apSSID))    strcpy(config.network.apSSID,    DEFAULT_AP_SSID);
    if (!strlen(config.network.ntpServer)) strcpy(config.network.ntpServer, DEFAULT_NTP_SERVER);

    if (!config.network.apIP[0]) {
        config.network.apIP[0]=192; config.network.apIP[1]=168;
        config.network.apIP[2]=4;   config.network.apIP[3]=1;
    }
    if (!config.network.apGateway[0]) {
        config.network.apGateway[0]=192; config.network.apGateway[1]=168;
        config.network.apGateway[2]=4;   config.network.apGateway[3]=1;
    }
    if (!config.network.apSubnet[0]) {
        config.network.apSubnet[0]=255; config.network.apSubnet[1]=255;
        config.network.apSubnet[2]=255; config.network.apSubnet[3]=0;
    }
    if (!config.network.subnet[0]) {
        config.network.subnet[0]=255; config.network.subnet[1]=255;
        config.network.subnet[2]=255; config.network.subnet[3]=0;
    }
    if (!config.network.staticIP[0]) {
        config.network.staticIP[0]=192; config.network.staticIP[1]=168;
        config.network.staticIP[2]=4;   config.network.staticIP[3]=100;
    }
    if (!config.network.gateway[0]) {
        config.network.gateway[0]=192; config.network.gateway[1]=168;
        config.network.gateway[2]=4;   config.network.gateway[3]=1;
    }
    if (!config.network.dns[0]) {
        config.network.dns[0]=8; config.network.dns[1]=8;
        config.network.dns[2]=8; config.network.dns[3]=8;
    }

    // ── Identity ──────────────────────────────────────────────────────────────
    if (!strlen(config.deviceName)) strcpy(config.deviceName, "Water Logger");

    // ── Theme ─────────────────────────────────────────────────────────────────
    if (!strlen(config.theme.primaryColor))      strcpy(config.theme.primaryColor,      "#275673");
    if (!strlen(config.theme.secondaryColor))    strcpy(config.theme.secondaryColor,    "#4a5568");
    if (!strlen(config.theme.bgColor))           strcpy(config.theme.bgColor,           "#f7fafc");
    if (!strlen(config.theme.textColor))         strcpy(config.theme.textColor,         "#2d3748");
    if (!strlen(config.theme.ffColor))           strcpy(config.theme.ffColor,           "#275673");
    if (!strlen(config.theme.pfColor))           strcpy(config.theme.pfColor,           "#7eb0d5");
    if (!strlen(config.theme.otherColor))        strcpy(config.theme.otherColor,        "#a0aec0");
    if (!strlen(config.theme.storageBarColor))   strcpy(config.theme.storageBarColor,   "#27ae60");
    if (!strlen(config.theme.storageBar70Color)) strcpy(config.theme.storageBar70Color, "#f39c12");
    if (!strlen(config.theme.storageBar90Color)) strcpy(config.theme.storageBar90Color, "#e74c3c");
    if (!strlen(config.theme.storageBarBorder))  strcpy(config.theme.storageBarBorder,  "#cccccc");
    if (!strlen(config.theme.chartLocalPath))    strcpy(config.theme.chartLocalPath,    "/chart.min.js");
}

// ============================================================================
// SANITIZE WAKE CONFIG
// ============================================================================
static bool sanitizeWakeConfig() {
    auto isRtcWakePinC3 = [](uint8_t pin) -> bool { return pin <= 5; };

    bool invalidPins = !isRtcWakePinC3(config.hardware.pinWakeupFF) ||
                       !isRtcWakePinC3(config.hardware.pinWakeupPF) ||
                       !isRtcWakePinC3(config.hardware.pinWifiTrigger);
    bool duplicatePins = (config.hardware.pinWakeupFF == config.hardware.pinWakeupPF) ||
                         (config.hardware.pinWakeupFF == config.hardware.pinWifiTrigger) ||
                         (config.hardware.pinWakeupPF == config.hardware.pinWifiTrigger);

    bool changed = false;

    if (invalidPins || duplicatePins) {
        config.hardware.pinWakeupFF    = DefaultPins::WAKEUP_FF;
        config.hardware.pinWakeupPF    = DefaultPins::WAKEUP_PF;
        config.hardware.pinWifiTrigger = DefaultPins::WIFI_TRIGGER;
        changed = true;
    }

    if (config.hardware.wakeupMode != WAKEUP_GPIO_ACTIVE_HIGH &&
        config.hardware.wakeupMode != WAKEUP_GPIO_ACTIVE_LOW) {
        config.hardware.wakeupMode = WAKEUP_GPIO_ACTIVE_HIGH;
        changed = true;
    }

    return changed;
}

// ============================================================================
// DEVICE ID
// ============================================================================
String generateDeviceId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[13];
    snprintf(id, sizeof(id), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    return String(id);
}

void regenerateDeviceId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[13];
    snprintf(id, sizeof(id), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    config.network.deviceId = String(id);
    saveConfig();
}

// ============================================================================
// LOAD DEFAULTS
// ============================================================================
void loadDefaultConfig() {
    memset(&config, 0, sizeof(DeviceConfig));
    config.magic   = CONFIG_STRUCT_MAGIC;
    config.version = CONFIG_VERSION;

    String devId = generateDeviceId();
    strncpy(config.deviceId,   devId.c_str(), sizeof(config.deviceId) - 1);
    strncpy(config.deviceName, "Water Logger", sizeof(config.deviceName) - 1);
    config.forceWebServer       = false;
    config.resetBootCountAction = -1;

    // Theme
    config.theme.mode = THEME_AUTO;
    strcpy(config.theme.primaryColor,      "#275673");
    strcpy(config.theme.secondaryColor,    "#4a5568");
    strcpy(config.theme.accentColor,       "#3182ce");
    strcpy(config.theme.bgColor,           "#f7fafc");
    strcpy(config.theme.textColor,         "#2d3748");
    strcpy(config.theme.ffColor,           "#275673");
    strcpy(config.theme.pfColor,           "#7eb0d5");
    strcpy(config.theme.otherColor,        "#a0aec0");
    strcpy(config.theme.storageBarColor,   "#27ae60");
    strcpy(config.theme.storageBar70Color, "#f39c12");
    strcpy(config.theme.storageBar90Color, "#e74c3c");
    strcpy(config.theme.storageBarBorder,  "#cccccc");
    config.theme.chartSource      = CHART_CDN;
    strcpy(config.theme.chartLocalPath, "/chart.min.js");
    config.theme.showIcons        = true;
    config.theme.chartLabelFormat = LABEL_DATETIME;

    // Datalog
    strcpy(config.datalog.prefix,      DEFAULT_DATALOG_PREFIX);
    strcpy(config.datalog.currentFile, "/datalog.txt");
    strcpy(config.datalog.folder,      "");
    config.datalog.rotation             = ROTATION_NONE;
    config.datalog.maxSizeKB            = 1024;
    config.datalog.maxEntries           = 10000;
    config.datalog.includeDeviceId      = false;
    config.datalog.timestampFilename    = true;
    config.datalog.dateFormat           = DATE_DDMMYYYY;
    config.datalog.timeFormat           = TIME_HHMMSS;
    config.datalog.endFormat            = END_TIME;
    config.datalog.volumeFormat         = VOL_L_COMMA;
    config.datalog.includeBootCount     = true;
    config.datalog.includeExtraPresses  = true;
    config.datalog.postCorrectionEnabled = true;
    config.datalog.pfToFfThreshold      = 4.5f;
    config.datalog.ffToPfThreshold      = 3.7f;
    config.datalog.manualPressThresholdMs = 500;

    // Flow meter
    config.flowMeter.pulsesPerLiter                = 450.0f;
    config.flowMeter.calibrationMultiplier         = 1.0f;
    config.flowMeter.monitoringWindowSecs          = 3;
    config.flowMeter.firstLoopMonitoringWindowSecs = 6;
    config.flowMeter.testMode                      = false;
    config.flowMeter.blinkDuration                 = 250;

    // Hardware
    config.hardware.version            = CONFIG_VERSION;
    config.hardware.storageType        = STORAGE_LITTLEFS;
    config.hardware.wakeupMode         = WAKEUP_GPIO_ACTIVE_HIGH;
    config.hardware.pinWifiTrigger     = DefaultPins::WIFI_TRIGGER;
    config.hardware.pinWakeupFF        = DefaultPins::WAKEUP_FF;
    config.hardware.pinWakeupPF        = DefaultPins::WAKEUP_PF;
    config.hardware.pinFlowSensor      = DefaultPins::FLOW_SENSOR;
    config.hardware.pinRtcCE           = DefaultPins::RTC_CE;
    config.hardware.pinRtcIO           = DefaultPins::RTC_IO;
    config.hardware.pinRtcSCLK        = DefaultPins::RTC_SCLK;
    config.hardware.pinSdCS            = DefaultPins::SD_CS;
    config.hardware.pinSdMOSI         = DefaultPins::SD_MOSI;
    config.hardware.pinSdMISO         = DefaultPins::SD_MISO;
    config.hardware.pinSdSCK          = DefaultPins::SD_SCK;
    config.hardware.cpuFreqMHz        = 80;
    config.hardware.debugMode         = false;
    config.hardware.defaultStorageView = 0;
    config.hardware.debounceMs        = 100;

    // Network
    config.network.wifiMode      = WIFIMODE_AP;
    strcpy(config.network.apSSID,     DEFAULT_AP_SSID);
    strcpy(config.network.apPassword, DEFAULT_AP_PASSWORD);
    strcpy(config.network.ntpServer,  DEFAULT_NTP_SERVER);
    config.network.timezone     = 2;
    config.network.useStaticIP  = false;
    config.network.staticIP[0]  = 192; config.network.staticIP[1]  = 168;
    config.network.staticIP[2]  = 4;   config.network.staticIP[3]  = 100;
    config.network.gateway[0]   = 192; config.network.gateway[1]   = 168;
    config.network.gateway[2]   = 4;   config.network.gateway[3]   = 1;
    config.network.subnet[0]    = 255; config.network.subnet[1]    = 255;
    config.network.subnet[2]    = 255; config.network.subnet[3]    = 0;
    config.network.dns[0]       = 8;   config.network.dns[1]       = 8;
    config.network.dns[2]       = 8;   config.network.dns[3]       = 8;
    config.network.apIP[0]      = 192; config.network.apIP[1]      = 168;
    config.network.apIP[2]      = 4;   config.network.apIP[3]      = 1;
    config.network.apGateway[0] = 192; config.network.apGateway[1] = 168;
    config.network.apGateway[2] = 4;   config.network.apGateway[3] = 1;
    config.network.apSubnet[0]  = 255; config.network.apSubnet[1]  = 255;
    config.network.apSubnet[2]  = 255; config.network.apSubnet[3]  = 0;
}

// ============================================================================
// MIGRATE
// ============================================================================
void migrateConfig(uint8_t fromVersion) {
    DBGF("Migrating config v%d -> v%d\n", fromVersion, CONFIG_VERSION);
    if (fromVersion < 6) {
        strcpy(config.network.ntpServer, DEFAULT_NTP_SERVER);
        config.network.timezone = 2;
    }
    if (fromVersion < 10) {
        config.datalog.postCorrectionEnabled  = true;
        config.datalog.pfToFfThreshold        = 4.5f;
        config.datalog.ffToPfThreshold        = 3.7f;
        config.datalog.manualPressThresholdMs = 500;
    }
    config.version = CONFIG_VERSION;
    saveConfig();
}

// ============================================================================
// LOAD CONFIG
// ============================================================================
bool loadConfig() {
    // ── Mount LittleFS ────────────────────────────────────────────────────────
    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        Serial.println("[CFG] LittleFS mount failed – using hardcoded defaults");
        loadDefaultConfig();
        return false;
    }

    // ── Open config file ──────────────────────────────────────────────────────
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f || f.isDirectory()) {
        Serial.println("[CFG] No config file – using hardcoded defaults");
        if (f) f.close();
        loadDefaultConfig();
        return false;
    }

    size_t fileSize = f.size();

    // ── Guard: file must be non-empty and not absurdly large ─────────────────
    if (fileSize == 0 || fileSize > sizeof(DeviceConfig) * 2) {
        Serial.printf("[CFG] Bad file size %u – using hardcoded defaults\n", fileSize);
        f.close();
        loadDefaultConfig();
        return false;
    }

    // ── Exact-size read (current struct version) ──────────────────────────────
    if (fileSize == sizeof(DeviceConfig)) {
        DeviceConfig tmp;
        size_t got = f.read((uint8_t*)&tmp, sizeof(DeviceConfig));
        f.close();

        if (got != sizeof(DeviceConfig) || tmp.magic != CONFIG_STRUCT_MAGIC) {
            Serial.println("[CFG] Magic mismatch / short read – using hardcoded defaults");
            loadDefaultConfig();
            return false;
        }
        memcpy(&config, &tmp, sizeof(DeviceConfig));
    }

    // ── Smaller file: migration from older struct ─────────────────────────────
    else if (fileSize < sizeof(DeviceConfig)) {
        uint8_t* rawBuf = (uint8_t*)malloc(fileSize);
        if (!rawBuf) {
            Serial.println("[CFG] malloc failed – using hardcoded defaults");
            f.close();
            loadDefaultConfig();
            return false;
        }

        size_t got = f.read(rawBuf, fileSize);
        f.close();

        uint32_t fileMagic = 0;
        if (got >= sizeof(uint32_t)) memcpy(&fileMagic, rawBuf, sizeof(uint32_t));

        if (got != fileSize || fileMagic != CONFIG_STRUCT_MAGIC) {
            free(rawBuf);
            Serial.println("[CFG] Corrupt file – using hardcoded defaults");
            loadDefaultConfig();
            return false;
        }

        // Start from safe defaults then overlay whatever bytes we have
        loadDefaultConfig();

        size_t headerSize      = offsetof(DeviceConfig, theme);
        size_t datalogOffset   = offsetof(DeviceConfig, datalog);
        size_t commonDatalogSz = offsetof(DatalogConfig, postCorrectionEnabled);

        if (headerSize <= fileSize)
            memcpy(&config, rawBuf, headerSize);

        if (offsetof(DeviceConfig, theme) + sizeof(ThemeConfig) <= fileSize)
            memcpy(&config.theme, rawBuf + offsetof(DeviceConfig, theme), sizeof(ThemeConfig));

        if (datalogOffset + commonDatalogSz <= fileSize)
            memcpy(&config.datalog, rawBuf + datalogOffset, commonDatalogSz);

        // FlowMeter
        size_t sizeDiff        = sizeof(DeviceConfig) - fileSize;
        size_t oldDatalogTotal = sizeof(DatalogConfig) - sizeDiff;
        size_t oldFlowOffset   = datalogOffset + oldDatalogTotal;
        if (oldFlowOffset + sizeof(FlowMeterConfig) <= fileSize)
            memcpy(&config.flowMeter, rawBuf + oldFlowOffset, sizeof(FlowMeterConfig));

        // Hardware
        size_t oldHWOffset = oldFlowOffset + sizeof(FlowMeterConfig);
        if (oldHWOffset + sizeof(HardwareConfig) <= fileSize)
            memcpy(&config.hardware, rawBuf + oldHWOffset, sizeof(HardwareConfig));

        // Network
        size_t oldNetOffset = oldHWOffset + sizeof(HardwareConfig);
        if (oldNetOffset < fileSize) {
            size_t avail = fileSize - oldNetOffset;
            size_t copy  = avail < sizeof(NetworkConfig) ? avail : sizeof(NetworkConfig);
            memcpy(&config.network, rawBuf + oldNetOffset, copy);
        }

        free(rawBuf);
        Serial.printf("[CFG] Migrated %u -> %u bytes\n",
                      (unsigned)fileSize, (unsigned)sizeof(DeviceConfig));
    }

    else {
        // fileSize > sizeof(DeviceConfig) — shouldn't happen, treat as corrupt
        f.close();
        Serial.println("[CFG] Oversized config file – using hardcoded defaults");
        loadDefaultConfig();
        return false;
    }

    // ── Version migration ─────────────────────────────────────────────────────
    if (config.version < CONFIG_VERSION) migrateConfig(config.version);

    // ── Sanitise wake pins ────────────────────────────────────────────────────
    if (sanitizeWakeConfig()) {
        Serial.println("[CFG] Wake pins invalid/duplicate – restored defaults");
        saveConfig();
    }

    // ── Null-termination guard on all critical char[] fields ─────────────────
    config.deviceName[sizeof(config.deviceName) - 1]                   = '\0';
    config.deviceId[sizeof(config.deviceId) - 1]                       = '\0';
    config.datalog.currentFile[sizeof(config.datalog.currentFile) - 1] = '\0';
    config.datalog.prefix[sizeof(config.datalog.prefix) - 1]           = '\0';
    config.datalog.folder[sizeof(config.datalog.folder) - 1]           = '\0';
    config.network.apSSID[sizeof(config.network.apSSID) - 1]           = '\0';
    config.network.apPassword[sizeof(config.network.apPassword) - 1]   = '\0';
    config.network.clientSSID[sizeof(config.network.clientSSID) - 1]   = '\0';
    config.network.ntpServer[sizeof(config.network.ntpServer) - 1]     = '\0';
    config.theme.primaryColor[sizeof(config.theme.primaryColor) - 1]   = '\0';

    // ── Fill any zero/empty fields that survived migration ────────────────────
    applyDefaults();

    Serial.printf("[CFG] Loaded v%u OK\n", config.version);
    return true;
}

// ============================================================================
// SAVE CONFIG
// ============================================================================
bool saveConfig() {
    config.magic   = CONFIG_STRUCT_MAGIC;
    config.version = CONFIG_VERSION;
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) { DBGLN("Failed to save config"); return false; }
    f.write((uint8_t*)&config, sizeof(DeviceConfig));
    f.close();
    DBGLN("Config saved");
    return true;
}
