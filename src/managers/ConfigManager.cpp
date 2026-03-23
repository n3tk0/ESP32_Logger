#include "ConfigManager.h"
#include "../core/Globals.h"
#include <LittleFS.h>
#include "esp_mac.h"
#include <math.h>

// Safe strcpy — always null-terminates (N25)
#define SAFE_STRCPY(dst, src) do { strncpy(dst, src, sizeof(dst) - 1); dst[sizeof(dst) - 1] = '\0'; } while(0)
// ============================================================================
// INTERNAL: apply safe defaults to any zero/empty fields
// Called after every load path so partially-migrated or corrupt-but-recoverable
// configs always surface sane values to the web UI.
// ============================================================================
static void applyDefaults() {
    auto badFloat = [](float v){ return v <= 0.0f || !isfinite(v); };

    // ── Flow meter ────────────────────────────────────────────────────────────
    if (badFloat(config.flowMeter.pulsesPerLiter))            config.flowMeter.pulsesPerLiter            = 450.0f;
    if (badFloat(config.flowMeter.calibrationMultiplier))     config.flowMeter.calibrationMultiplier     = 1.0f;
    if (config.flowMeter.monitoringWindowSecs      <= 0) config.flowMeter.monitoringWindowSecs      = 3;
    if (config.flowMeter.firstLoopMonitoringWindowSecs <= 0) config.flowMeter.firstLoopMonitoringWindowSecs = 6;
    if (config.flowMeter.blinkDuration             <= 0) config.flowMeter.blinkDuration             = 250;

    // ── Hardware ──────────────────────────────────────────────────────────────
    if (config.hardware.cpuFreqMHz  <= 0) config.hardware.cpuFreqMHz  = 80;
    if (config.hardware.debounceMs  == 0) config.hardware.debounceMs  = 100;

    // ── Datalog ───────────────────────────────────────────────────────────────
    if (badFloat(config.datalog.pfToFfThreshold)) config.datalog.pfToFfThreshold = 4.5f;
    if (badFloat(config.datalog.ffToPfThreshold)) config.datalog.ffToPfThreshold = 3.7f;
    if (config.datalog.maxSizeKB       == 0) config.datalog.maxSizeKB       = 1024;
    
    if (config.datalog.maxEntries == 0) {
        config.datalog.maxEntries = 10000;
        config.datalog.includeBootCount = true;
        config.datalog.includeExtraPresses = true;
        config.datalog.postCorrectionEnabled = true;
        config.datalog.timestampFilename = true;
    }
    
    if (config.datalog.manualPressThresholdMs == 0) config.datalog.manualPressThresholdMs = 500;
    if (!strlen(config.datalog.prefix))       SAFE_STRCPY(config.datalog.prefix,      DEFAULT_DATALOG_PREFIX);
    if (!strlen(config.datalog.currentFile))  SAFE_STRCPY(config.datalog.currentFile, "/datalog.txt");

    // ── Network ───────────────────────────────────────────────────────────────
    if (!strlen(config.network.apSSID))     SAFE_STRCPY(config.network.apSSID,    DEFAULT_AP_SSID);
    if (!strlen(config.network.apPassword)) SAFE_STRCPY(config.network.apPassword, DEFAULT_AP_PASSWORD);
    if (!strlen(config.network.ntpServer)) SAFE_STRCPY(config.network.ntpServer, DEFAULT_NTP_SERVER);

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
    if (!strlen(config.deviceName)) SAFE_STRCPY(config.deviceName, "Water Logger");

    // ── Theme ─────────────────────────────────────────────────────────────────
    if (!strlen(config.theme.primaryColor))      SAFE_STRCPY(config.theme.primaryColor,      "#275673");
    if (!strlen(config.theme.secondaryColor))    SAFE_STRCPY(config.theme.secondaryColor,    "#4a5568");
    if (!strlen(config.theme.accentColor))       SAFE_STRCPY(config.theme.accentColor,       "#3182ce");
    if (!strlen(config.theme.lightBgColor))      SAFE_STRCPY(config.theme.lightBgColor,      "#f0f2f5");
    if (!strlen(config.theme.lightTextColor))    SAFE_STRCPY(config.theme.lightTextColor,    "#2d3748");
    if (!strlen(config.theme.darkBgColor))       SAFE_STRCPY(config.theme.darkBgColor,       "#0f172a");
    if (!strlen(config.theme.darkTextColor))     SAFE_STRCPY(config.theme.darkTextColor,     "#e2e8f0");
    if (!strlen(config.theme.ffColor))           SAFE_STRCPY(config.theme.ffColor,           "#275673");
    if (!strlen(config.theme.pfColor))           SAFE_STRCPY(config.theme.pfColor,           "#7eb0d5");
    if (!strlen(config.theme.otherColor))        SAFE_STRCPY(config.theme.otherColor,        "#a0aec0");
    if (!strlen(config.theme.storageBarColor))   SAFE_STRCPY(config.theme.storageBarColor,   "#27ae60");
    if (!strlen(config.theme.storageBar70Color)) SAFE_STRCPY(config.theme.storageBar70Color, "#f39c12");
    if (!strlen(config.theme.storageBar90Color)) SAFE_STRCPY(config.theme.storageBar90Color, "#e74c3c");
    if (!strlen(config.theme.storageBarBorder))  SAFE_STRCPY(config.theme.storageBarBorder,  "#cccccc");
    if (!strlen(config.theme.chartLocalPath))    SAFE_STRCPY(config.theme.chartLocalPath,    "/chart.min.js");
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

    auto isSafePinC3 = [](uint8_t pin) -> bool { return pin <= 21 && !(pin >= 11 && pin <= 17); };

    if (!isSafePinC3(config.hardware.pinFlowSensor) || config.hardware.pinFlowSensor == 0) {
        config.hardware.pinFlowSensor = DefaultPins::FLOW_SENSOR;
        changed = true;
    }

    if (!isSafePinC3(config.hardware.pinRtcCE) || 
        !isSafePinC3(config.hardware.pinRtcIO) || 
        !isSafePinC3(config.hardware.pinRtcSCLK)) {
        config.hardware.pinRtcCE   = DefaultPins::RTC_CE;
        config.hardware.pinRtcIO   = DefaultPins::RTC_IO;
        config.hardware.pinRtcSCLK = DefaultPins::RTC_SCLK;
        changed = true;
    }

    if (config.hardware.cpuFreqMHz != 80 && config.hardware.cpuFreqMHz != 160) {
        config.hardware.cpuFreqMHz = 80;
        changed = true;
    }

    if (config.hardware.wakeupMode != WAKEUP_GPIO_ACTIVE_HIGH &&
        config.hardware.wakeupMode != WAKEUP_GPIO_ACTIVE_LOW) {
        config.hardware.wakeupMode = WAKEUP_GPIO_ACTIVE_HIGH;
        changed = true;
    }

    if (isnan(config.flowMeter.pulsesPerLiter) || isinf(config.flowMeter.pulsesPerLiter) || config.flowMeter.pulsesPerLiter <= 0.0f) {
        config.flowMeter.pulsesPerLiter = 450.0f;
        changed = true;
    }

    if (isnan(config.flowMeter.calibrationMultiplier) || isinf(config.flowMeter.calibrationMultiplier) || config.flowMeter.calibrationMultiplier <= 0.0f) {
        config.flowMeter.calibrationMultiplier = 1.0f;
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
    strncpy(config.deviceId, id, sizeof(config.deviceId) - 1);
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
    SAFE_STRCPY(config.theme.primaryColor,      "#275673");
    SAFE_STRCPY(config.theme.secondaryColor,    "#4a5568");
    SAFE_STRCPY(config.theme.accentColor,       "#3182ce");
    SAFE_STRCPY(config.theme.lightBgColor,           "#f0f2f5");
    SAFE_STRCPY(config.theme.lightTextColor,         "#2d3748");
    SAFE_STRCPY(config.theme.darkBgColor,            "#0f172a");
    SAFE_STRCPY(config.theme.darkTextColor,          "#e2e8f0");
    SAFE_STRCPY(config.theme.ffColor,           "#275673");
    SAFE_STRCPY(config.theme.pfColor,           "#7eb0d5");
    SAFE_STRCPY(config.theme.otherColor,        "#a0aec0");
    SAFE_STRCPY(config.theme.storageBarColor,   "#27ae60");
    SAFE_STRCPY(config.theme.storageBar70Color, "#f39c12");
    SAFE_STRCPY(config.theme.storageBar90Color, "#e74c3c");
    SAFE_STRCPY(config.theme.storageBarBorder,  "#cccccc");
    config.theme.chartSource      = CHART_CDN;
    SAFE_STRCPY(config.theme.chartLocalPath, "/chart.min.js");
    config.theme.showIcons        = true;
    config.theme.chartLabelFormat = LABEL_DATETIME;

    // Datalog
    SAFE_STRCPY(config.datalog.prefix,      DEFAULT_DATALOG_PREFIX);
    SAFE_STRCPY(config.datalog.currentFile, "/datalog.txt");
    SAFE_STRCPY(config.datalog.folder,      "");
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
    SAFE_STRCPY(config.network.apSSID,     DEFAULT_AP_SSID);
    SAFE_STRCPY(config.network.apPassword, DEFAULT_AP_PASSWORD);
    SAFE_STRCPY(config.network.ntpServer,  DEFAULT_NTP_SERVER);
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
    if (fromVersion > CONFIG_VERSION) {
        Serial.printf("[Config] WARN: config v%d > firmware v%d — resetting defaults\n", fromVersion, CONFIG_VERSION);
        resetConfig();
        return;
    }
    if (fromVersion < 6) {
        SAFE_STRCPY(config.network.ntpServer, DEFAULT_NTP_SERVER);
        config.network.timezone = 2;
    }
    if (fromVersion < 10) {
        config.datalog.postCorrectionEnabled  = true;
        config.datalog.pfToFfThreshold        = 4.5f;
        config.datalog.ffToPfThreshold        = 3.7f;
        config.datalog.manualPressThresholdMs = 500;
    }
    if (fromVersion < 11) {
        config.theme.showIcons = true;
        config.theme.chartLabelFormat = LABEL_DATETIME;
    }
    if (fromVersion < 12) {
        config.hardware.defaultStorageView = 0;
        if (!strlen(config.network.apPassword)) SAFE_STRCPY(config.network.apPassword, DEFAULT_AP_PASSWORD);
    }
    config.version = CONFIG_VERSION;
    config.hardware.version = CONFIG_VERSION;
    saveConfig();
}

// ============================================================================
// LOAD CONFIG
// ============================================================================
bool loadConfig() {
    // ── Mount LittleFS ────────────────────────────────────────────────────────
    if (LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        littleFsAvailable = true;
    } else {
        Serial.println("[CFG] LittleFS mount failed – using hardcoded defaults");
        loadDefaultConfig();
        return false;
    }

    // ── Recover interrupted crash-safe write ─────────────────────────────────
    if (!LittleFS.exists(CONFIG_FILE) && LittleFS.exists("/config.tmp")) {
        Serial.println("[CFG] Recovering config from temp file");
        LittleFS.rename("/config.tmp", CONFIG_FILE);
    } else if (LittleFS.exists("/config.tmp")) {
        LittleFS.remove("/config.tmp");   // stale temp, real file exists
    }

    // ── Open config file ──────────────────────────────────────────────────────
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f || f.isDirectory()) {
        Serial.println("[CFG] No config file – using hardcoded defaults");
        if (f) f.close();
        loadDefaultConfig();
        saveConfig();
        return false;
    }

    size_t fileSize = f.size();

    // ── Guard: file must be non-empty and not absurdly large ─────────────────
    if (fileSize == 0 || fileSize > sizeof(DeviceConfig) * 2) {
        Serial.printf("[CFG] Bad file size %u – using hardcoded defaults\n", fileSize);
        f.close();
        loadDefaultConfig();
        saveConfig();
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
            saveConfig();
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
            saveConfig();
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
            saveConfig();
            return false;
        }

        // Start from safe defaults then overlay whatever bytes we have
        loadDefaultConfig();

        size_t headerSize = offsetof(DeviceConfig, theme);
        if (headerSize <= fileSize)
            memcpy(&config, rawBuf, headerSize);

        #define SAFE_COPY(field) \
            if (offsetof(DeviceConfig, field) + sizeof(config.field) <= fileSize) \
                memcpy(&config.field, rawBuf + offsetof(DeviceConfig, field), sizeof(config.field)); \
            else if (offsetof(DeviceConfig, field) < fileSize) \
                memcpy(&config.field, rawBuf + offsetof(DeviceConfig, field), fileSize - offsetof(DeviceConfig, field))

        SAFE_COPY(theme);
        SAFE_COPY(datalog);
        SAFE_COPY(flowMeter);
        SAFE_COPY(hardware);
        SAFE_COPY(network);

        #undef SAFE_COPY

        free(rawBuf);
        Serial.printf("[CFG] Migrated %u -> %u bytes\n",
                      (unsigned)fileSize, (unsigned)sizeof(DeviceConfig));
    }

    else {
        // fileSize > sizeof(DeviceConfig) — shouldn't happen, treat as corrupt
        f.close();
        Serial.println("[CFG] Oversized config file – using hardcoded defaults");
        loadDefaultConfig();
        saveConfig();
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
    config.network.clientPassword[sizeof(config.network.clientPassword) - 1] = '\0';
    config.network.ntpServer[sizeof(config.network.ntpServer) - 1]     = '\0';
    config.theme.primaryColor[sizeof(config.theme.primaryColor) - 1]   = '\0';
    config.theme.secondaryColor[sizeof(config.theme.secondaryColor) - 1] = '\0';
    config.theme.lightBgColor[sizeof(config.theme.lightBgColor) - 1]   = '\0';
    config.theme.lightTextColor[sizeof(config.theme.lightTextColor) - 1] = '\0';
    config.theme.darkBgColor[sizeof(config.theme.darkBgColor) - 1]     = '\0';
    config.theme.darkTextColor[sizeof(config.theme.darkTextColor) - 1] = '\0';
    config.theme.ffColor[sizeof(config.theme.ffColor) - 1]             = '\0';
    config.theme.pfColor[sizeof(config.theme.pfColor) - 1]             = '\0';
    config.theme.otherColor[sizeof(config.theme.otherColor) - 1]       = '\0';
    config.theme.storageBarColor[sizeof(config.theme.storageBarColor) - 1] = '\0';
    config.theme.storageBar70Color[sizeof(config.theme.storageBar70Color) - 1] = '\0';
    config.theme.storageBar90Color[sizeof(config.theme.storageBar90Color) - 1] = '\0';
    config.theme.storageBarBorder[sizeof(config.theme.storageBarBorder) - 1] = '\0';
    config.theme.chartLocalPath[sizeof(config.theme.chartLocalPath) - 1] = '\0';

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

    // Crash-safe write: write to temp file, then rename over the real one.
    // If power is lost during write, the original config.bin remains intact.
    static const char* TMP_FILE = "/config.tmp";

    File f = LittleFS.open(TMP_FILE, "w");
    if (!f) { DBGLN("Failed to open config temp file"); return false; }
    size_t written = f.write((uint8_t*)&config, sizeof(DeviceConfig));
    f.close();

    if (written != sizeof(DeviceConfig)) {
        DBGLN("Config write incomplete");
        LittleFS.remove(TMP_FILE);
        return false;
    }

    // Atomic rename: LittleFS rename overwrites the destination
    if (!LittleFS.rename(TMP_FILE, CONFIG_FILE)) {
        DBGLN("Config rename failed");
        return false;
    }

    DBGLN("Config saved");
    return true;
}
