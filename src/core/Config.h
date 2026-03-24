#pragma once

#include <Arduino.h>

// ============================================================================
// VERSION  –  single source of truth
// ============================================================================
#define VERSION_MAJOR 4
#define VERSION_MINOR 2
#define VERSION_PATCH 0

// Inline so every translation unit that includes Config.h gets the same
// string without needing Utils.h
inline String getVersionString() {
    char buf[16];
    snprintf(buf, sizeof(buf), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    return String(buf);
}

// Debug mode switch is declared in Logger.ino for easier tuning in one place.
extern bool gDebugMode;

#define DBG(x)      do { if (gDebugMode) Serial.print(x); } while (0)
#define DBGLN(x)    do { if (gDebugMode) Serial.println(x); } while (0)
#define DBGF(...)   do { if (gDebugMode) Serial.printf(__VA_ARGS__); } while (0)

// ============================================================================
// CONSTANTS
// ============================================================================
constexpr const char* CONFIG_FILE            = "/config.bin";
constexpr const char* BOOTCOUNT_BACKUP_FILE  = "/bootcount.bin";
constexpr const char* DEFAULT_AP_SSID        = "WaterLogger";
constexpr const char* DEFAULT_AP_PASSWORD    = "water12345";
constexpr const char* DEFAULT_DATALOG_PREFIX = "datalog";
constexpr const char* DEFAULT_NTP_SERVER     = "pool.ntp.org";

const unsigned long TEST_MODE_BLINK_MS      = 250;
const unsigned long TEST_MODE_HOLD_MS       = 1000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const int           LOG_BATCH_SIZE          = 16;

// ISR Debounce for flow sensor
const unsigned long ISR_DEBOUNCE_MICROS = 1000;  // 1ms

// State machine timing
const unsigned long BUTTON_WAIT_FLOW_MS  = 6000;
const unsigned long FLOW_IDLE_TIMEOUT_MS = 3000;

#define CONFIG_STRUCT_MAGIC  0xC0FFEE36
#define CONFIG_VERSION       12

// DS1302 RAM addresses for bootcount backup
#define RTC_RAM_BOOTCOUNT_ADDR  0
#define RTC_RAM_MAGIC_ADDR      4
#define RTC_RAM_MAGIC_VALUE     0xBC

// ============================================================================
// DEFAULT PIN DEFINITIONS – XIAO ESP32-C3
// ============================================================================
namespace DefaultPins {
    constexpr uint8_t WIFI_TRIGGER = 2;
    constexpr uint8_t WAKEUP_FF   = 3;
    constexpr uint8_t WAKEUP_PF   = 4;
    constexpr uint8_t FLOW_SENSOR = 21;
    constexpr uint8_t RTC_CE      = 5;
    constexpr uint8_t RTC_IO      = 6;
    constexpr uint8_t RTC_SCLK   = 7;
    constexpr uint8_t SD_CS       = 10;
    constexpr uint8_t SD_MOSI     = 11;
    constexpr uint8_t SD_MISO     = 12;
    constexpr uint8_t SD_SCK      = 13;
}

// ============================================================================
// ENUMERATIONS
// ============================================================================
enum StorageType   : uint8_t { STORAGE_LITTLEFS = 0, STORAGE_SD_CARD = 1 };
enum WiFiModeType  : uint8_t { WIFIMODE_AP = 0, WIFIMODE_CLIENT = 1 };
enum ThemeMode     : uint8_t { THEME_LIGHT = 0, THEME_DARK = 1, THEME_AUTO = 2 };
enum ChartSource   : uint8_t { CHART_LOCAL = 0, CHART_CDN = 1 };
enum WakeupMode    : uint8_t { WAKEUP_GPIO_ACTIVE_HIGH = 0, WAKEUP_GPIO_ACTIVE_LOW = 1 };

enum DatalogRotation : uint8_t {
    ROTATION_NONE = 0, ROTATION_DAILY = 1,
    ROTATION_WEEKLY = 2, ROTATION_MONTHLY = 3, ROTATION_SIZE = 4
};

enum ChartLabelFormat : uint8_t {
    LABEL_DATETIME = 0, LABEL_BOOTCOUNT = 1, LABEL_BOTH = 2
};

enum DateFormat   : uint8_t { DATE_OFF=0, DATE_DDMMYYYY=1, DATE_MMDDYYYY=2, DATE_YYYYMMDD=3, DATE_DDMMYYYY_DOT=4 };
enum TimeFormat   : uint8_t { TIME_HHMMSS=0, TIME_HHMM=1, TIME_12H=2 };
enum EndFormat    : uint8_t { END_TIME=0, END_DURATION=1, END_OFF=2 };
enum VolumeFormat : uint8_t { VOL_L_COMMA=0, VOL_L_DOT=1, VOL_NUM_ONLY=2, VOL_OFF=3 };

// ============================================================================
// LOGGING STATE MACHINE
// ============================================================================
enum LoggingState {
    STATE_IDLE,
    STATE_WAIT_FLOW,
    STATE_MONITORING,
    STATE_DONE
};

// ============================================================================
// CONFIG STRUCTURES
// ============================================================================
#pragma pack(push, 1)

struct ThemeConfig {
    ThemeMode mode;
    char primaryColor[8];
    char secondaryColor[8];
    char accentColor[8];
    char lightBgColor[8];
    char lightTextColor[8];
    char darkBgColor[8];
    char darkTextColor[8];
    char ffColor[8];
    char pfColor[8];
    char otherColor[8];
    char storageBarColor[8];
    char storageBar70Color[8];
    char storageBar90Color[8];
    char storageBarBorder[8];
    char logoSource[129];
    char faviconPath[33];
    char boardDiagramPath[65];
    ChartSource chartSource;
    char chartLocalPath[65];
    bool showIcons;
    ChartLabelFormat chartLabelFormat;
};

struct DatalogConfig {
    char prefix[33];
    char currentFile[65];
    char folder[33];
    DatalogRotation rotation;
    uint32_t maxSizeKB;
    uint16_t maxEntries;
    bool includeDeviceId;
    bool timestampFilename;
    uint8_t dateFormat;
    uint8_t timeFormat;
    uint8_t endFormat;
    uint8_t volumeFormat;
    bool includeBootCount;
    bool includeExtraPresses;
    // v4.1.3+ Post-correction
    bool postCorrectionEnabled;
    float pfToFfThreshold;
    float ffToPfThreshold;
    // v4.1.4+ Hold threshold
    uint16_t manualPressThresholdMs;
};

struct FlowMeterConfig {
    float pulsesPerLiter;
    float calibrationMultiplier;
    int monitoringWindowSecs;
    int firstLoopMonitoringWindowSecs;
    bool testMode;
    int blinkDuration;
    uint8_t reserved[8];
};

struct HardwareConfig {
    uint8_t version;
    StorageType storageType;
    WakeupMode wakeupMode;
    uint8_t pinWifiTrigger;
    uint8_t pinWakeupFF;
    uint8_t pinWakeupPF;
    uint8_t pinFlowSensor;
    uint8_t pinRtcCE;
    uint8_t pinRtcIO;
    uint8_t pinRtcSCLK;
    uint8_t pinSdCS;
    uint8_t pinSdMOSI;
    uint8_t pinSdMISO;
    uint8_t pinSdSCK;
    int cpuFreqMHz;
    bool debugMode;
    uint8_t defaultStorageView;
    uint16_t debounceMs;
    uint8_t reserved[5];
};

struct NetworkConfig {
    WiFiModeType wifiMode;
    char apSSID[33];
    char apPassword[65];
    char clientSSID[33];
    char clientPassword[65];
    bool useStaticIP;
    uint8_t staticIP[4];
    uint8_t gateway[4];
    uint8_t subnet[4];
    uint8_t dns[4];
    char ntpServer[65];
    int8_t timezone;
    int8_t dstOffsetHours;     // DST offset in hours (0 or 1 typically)
    uint8_t apIP[4];
    uint8_t apGateway[4];
    uint8_t apSubnet[4];
    uint8_t reserved[17]; // Reserved for alignment (one byte used for dstOffsetHours)
};

struct DeviceConfig {
    uint32_t magic;
    uint8_t version;
    char deviceId[13];
    char deviceName[33];
    uint8_t _reserved_lang;
    bool forceWebServer;
    int8_t resetBootCountAction;
    ThemeConfig    theme;
    DatalogConfig  datalog;
    FlowMeterConfig flowMeter;
    HardwareConfig hardware;
    NetworkConfig  network;
};

struct LogEntry {
    uint32_t wakeTimestamp;
    uint32_t sleepTimestamp;
    uint16_t bootCount;
    uint16_t ffCount;
    uint16_t pfCount;
    float    volumeLiters;
    char     wakeupReason[10];
};

static_assert(sizeof(LogEntry) * LOG_BATCH_SIZE + 32 < 512, "RTC buffer exceeds safe budget");

#pragma pack(pop)
