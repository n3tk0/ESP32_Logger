#pragma once

#include <Arduino.h>

// All build-time tunables (module toggles, debug flags, task tuning,
// timeouts, batch sizes) live in src/setup.h.  We pull them in here so
// every file that includes Config.h transparently gets them too.
#include "../setup.h"

// PIN_UNSET sentinel + BoardProfile forward decl. Header is stdint-only;
// no transitive heavy includes.
#include "BoardProfiles.h"

// ============================================================================
// VERSION  –  single source of truth
// ============================================================================
// R28 / AUDIT 31.11: Config.h is the canonical source. ESP_Logger.ino's banner
// comment and www/changelog.txt's top entry must reference this same triple.
// Bumped 4.2.0 -> 4.2.1 to match the latest changelog entry (safe-mode + OTA
// rollback hardening), which previously diverged from Config.h.
#define VERSION_MAJOR 4
#define VERSION_MINOR 2
#define VERSION_PATCH 1

// Inline so every translation unit that includes Config.h gets the same
// string without needing Utils.h
inline String getVersionString() {
    char buf[16];
    snprintf(buf, sizeof(buf), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    return String(buf);
}

// DBG/DBGLN/DBGF resolve to no-ops when DEBUG_MODE == 0 (set in setup.h).
#if DEBUG_MODE
  #define DBG(x)      Serial.print(x)
  #define DBGLN(x)    Serial.println(x)
  #define DBGF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

// ============================================================================
// CONSTANTS — file paths, default network identity
// (numeric/timeout tunables now live in setup.h)
// ============================================================================
constexpr const char* CONFIG_FILE            = "/config.bin";
constexpr const char* BOOTCOUNT_BACKUP_FILE  = "/bootcount.bin";
constexpr const char* DEFAULT_AP_SSID        = "WaterLogger";
constexpr const char* DEFAULT_AP_PASSWORD    = "water12345";
constexpr const char* DEFAULT_DATALOG_PREFIX = "datalog";
constexpr const char* DEFAULT_NTP_SERVER     = "pool.ntp.org";

#define CONFIG_STRUCT_MAGIC  0xC0FFEE36
#define CONFIG_VERSION       13

// DS1302 RAM addresses for bootcount backup
#define RTC_RAM_BOOTCOUNT_ADDR  0
#define RTC_RAM_MAGIC_ADDR      4
#define RTC_RAM_MAGIC_VALUE     0xBC

// ============================================================================
// DEFAULT PIN DEFINITIONS — R11: all unset, wizard must assign
// ----------------------------------------------------------------------------
// Was hardcoded XIAO-C3-specific values that collided with strap pins
// (5, 8, 9), SPI flash bus (10-13), and USB CDC (18, 19, 21). Closes
// AUDIT 5.3, 5.7, 5.8, 5.9, 23.1, 31.2, 31.3.
//
// New devices boot with every pin == PIN_UNSET → g_setupRequired=true
// → first-run wizard runs. Existing devices keep their saved config.bin
// values; the wizard scans them against the chosen board profile and
// flags violations.
// ============================================================================
namespace DefaultPins {
    constexpr uint8_t WIFI_TRIGGER = PIN_UNSET;
    constexpr uint8_t WAKEUP_FF    = PIN_UNSET;
    constexpr uint8_t WAKEUP_PF    = PIN_UNSET;
    constexpr uint8_t FLOW_SENSOR  = PIN_UNSET;
    constexpr uint8_t RTC_CE       = PIN_UNSET;
    constexpr uint8_t RTC_IO       = PIN_UNSET;
    constexpr uint8_t RTC_SCLK     = PIN_UNSET;
    constexpr uint8_t SD_CS        = PIN_UNSET;
    constexpr uint8_t SD_MOSI      = PIN_UNSET;
    constexpr uint8_t SD_MISO      = PIN_UNSET;
    constexpr uint8_t SD_SCK       = PIN_UNSET;
}

// ============================================================================
// ENUMERATIONS
// ============================================================================
enum StorageType   : uint8_t { STORAGE_LITTLEFS = 0, STORAGE_SD_CARD = 1 };
enum WiFiModeType  : uint8_t { WIFIMODE_AP = 0, WIFIMODE_CLIENT = 1 };
enum ThemeMode     : uint8_t { THEME_LIGHT = 0, THEME_DARK = 1, THEME_AUTO = 2 };
enum ChartSource   : uint8_t { CHART_LOCAL = 0, CHART_CDN = 1 };
enum WakeupMode    : uint8_t { WAKEUP_GPIO_ACTIVE_HIGH = 0, WAKEUP_GPIO_ACTIVE_LOW = 1 };
enum PlatformMode  : uint8_t { PLATFORM_LEGACY = 0, PLATFORM_CONTINUOUS = 1, PLATFORM_HYBRID = 2 };

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

// LoggerConfig — wide-CSV pipeline + sensor logging knobs (v13).
//
// IMPORTANT: this struct lives at the END of DeviceConfig.  The on-disk
// migration in ConfigManager::loadConfig() uses offsetof-based safe-copy
// to forward-migrate older binary configs; appending here means the
// offsets of every pre-v13 field stay stable and old binaries keep
// loading cleanly.  Future fields go at the end of LoggerConfig (or
// inside the `reserved` tail if size-stable padding is needed).
struct LoggerConfig {
    bool     csvLoggingEnabled;          // wide-CSV pipeline kill switch (default on)
    uint16_t aggregationIntervalSec;     // RAM aggregator flush cadence (sec)
    bool     humidityCorrectionEnabled;  // SDS011 k-Köhler correction
    float    humidityCorrectionKappa;    // default 0.35
    uint8_t  reserved[16];               // future v13+ fields without ABI break
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
    LoggerConfig   logger;     // appended in v13 — keep at the end
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