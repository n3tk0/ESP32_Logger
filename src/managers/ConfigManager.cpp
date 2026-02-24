#include "ConfigManager.h"
#include "../core/Globals.h"
#include <LittleFS.h>
#include "esp_mac.h"


static bool sanitizeHardwareConfig() {
    auto isRtcWakePinC3 = [](uint8_t pin) -> bool { return pin <= 5; };
    auto isPinSafe = [](uint8_t pin) -> bool {
        if (pin > 21) return false;
        if (pin >= 11 && pin <= 17) return false;
        return true;
    };
    auto isRtcDataPinValid = [&](uint8_t pin) -> bool {
        return pin != 0 && isPinSafe(pin);
    };

    bool changed = false;

    bool invalidWakePins = !isRtcWakePinC3(config.hardware.pinWakeupFF) ||
                           !isRtcWakePinC3(config.hardware.pinWakeupPF) ||
                           !isRtcWakePinC3(config.hardware.pinWifiTrigger);
    bool duplicateWakePins = (config.hardware.pinWakeupFF == config.hardware.pinWakeupPF) ||
                             (config.hardware.pinWakeupFF == config.hardware.pinWifiTrigger) ||
                             (config.hardware.pinWakeupPF == config.hardware.pinWifiTrigger);
    if (invalidWakePins || duplicateWakePins) {
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

    if (!isRtcDataPinValid(config.hardware.pinRtcCE)) {
        config.hardware.pinRtcCE = DefaultPins::RTC_CE;
        changed = true;
    }
    if (!isRtcDataPinValid(config.hardware.pinRtcIO)) {
        config.hardware.pinRtcIO = DefaultPins::RTC_IO;
        changed = true;
    }
    if (!isRtcDataPinValid(config.hardware.pinRtcSCLK)) {
        config.hardware.pinRtcSCLK = DefaultPins::RTC_SCLK;
        changed = true;
    }

    return changed;
}

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
    config.flowMeter.pulsesPerLiter                 = 450.0f;
    config.flowMeter.calibrationMultiplier          = 1.0f;
    config.flowMeter.monitoringWindowSecs           = 3;
    config.flowMeter.firstLoopMonitoringWindowSecs  = 6;
    config.flowMeter.testMode                       = false;
    config.flowMeter.blinkDuration                  = 250;

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

bool loadConfig() {
    // Explicitly using "spiffs" label for maximum compatibility on ESP32
    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        DBGLN("LittleFS mount failed (loadConfig)");
        loadDefaultConfig();
        return false;
    }
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
        DBGLN("No config file, loading defaults");
        loadDefaultConfig();
        return false;
    }

    size_t fileSize = f.size();

    if (fileSize == sizeof(DeviceConfig)) {
        size_t readBytes = f.read((uint8_t*)&config, sizeof(DeviceConfig));
        f.close();
        if (readBytes != sizeof(DeviceConfig) || config.magic != CONFIG_STRUCT_MAGIC) {
            DBGLN("Invalid config, loading defaults");
            loadDefaultConfig();
            return false;
        }
    } else if (fileSize > 0 && fileSize < sizeof(DeviceConfig)) {
        // Struct migration from older version
        uint8_t* rawBuf = (uint8_t*)malloc(fileSize);
        if (!rawBuf) { f.close(); loadDefaultConfig(); return false; }

        size_t readBytes = f.read(rawBuf, fileSize);
        f.close();

        uint32_t fileMagic;
        memcpy(&fileMagic, rawBuf, sizeof(uint32_t));
        if (readBytes != fileSize || fileMagic != CONFIG_STRUCT_MAGIC) {
            free(rawBuf);
            loadDefaultConfig();
            return false;
        }

        loadDefaultConfig();

        size_t datalogOffset = offsetof(DeviceConfig, datalog);
        size_t headerSize    = offsetof(DeviceConfig, theme);
        memcpy(&config,       rawBuf,                  headerSize);
        memcpy(&config.theme, rawBuf + offsetof(DeviceConfig, theme), sizeof(ThemeConfig));

        size_t commonDatalogSize = offsetof(DatalogConfig, postCorrectionEnabled);
        memcpy(&config.datalog, rawBuf + datalogOffset, commonDatalogSize);

        size_t sizeDiff          = sizeof(DeviceConfig) - fileSize;
        size_t oldDatalogTotal   = sizeof(DatalogConfig) - sizeDiff;
        size_t oldFlowOffset     = datalogOffset + oldDatalogTotal;
        if (oldFlowOffset + sizeof(FlowMeterConfig) <= fileSize)
            memcpy(&config.flowMeter, rawBuf + oldFlowOffset, sizeof(FlowMeterConfig));

        size_t oldHWOffset = oldFlowOffset + sizeof(FlowMeterConfig);
        if (oldHWOffset + sizeof(HardwareConfig) <= fileSize)
            memcpy(&config.hardware, rawBuf + oldHWOffset, sizeof(HardwareConfig));

        size_t oldNetOffset = oldHWOffset + sizeof(HardwareConfig);
        size_t netAvail     = (oldNetOffset < fileSize) ? (fileSize - oldNetOffset) : 0;
        if (netAvail > 0) {
            size_t copySize = (netAvail < sizeof(NetworkConfig)) ? netAvail : sizeof(NetworkConfig);
            memcpy(&config.network, rawBuf + oldNetOffset, copySize);
        }

        free(rawBuf);
        DBGF("Config migrated: %d -> %d bytes\n", fileSize, sizeof(DeviceConfig));
    } else {
        f.close();
        loadDefaultConfig();
        return false;
    }

    if (config.version < CONFIG_VERSION) migrateConfig(config.version);

    if (sanitizeHardwareConfig()) {
        DBGLN("Config hardware pins invalid -> restored defaults");
        saveConfig();
    }

    DBGF("Config loaded v%d\n", config.version);
    return true;
}

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
