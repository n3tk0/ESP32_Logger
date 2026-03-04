/**************************************************************************************************
 * PROJECT: ESP32 Low-Power Water Usage Logger v4.1
 * VERSION: 4.1.4
 * TARGET: XIAO ESP32-C3 (RISC-V)
 * 
 * DESCRIPTION: Multi-sensor water logging with theme support, configurable datalog,
 *              file management, and ESP32-C3 specific deep sleep GPIO wake-up
 * 
 * CHANGELOG:
 *   v4.1.4  - Post-correction improvements & UI polish:
 *           - Button hold duration measurement for post-correction filtering
 *           - Extended button hold skips correction (intentional extra volume)
 *           - manualPressThresholdMs configurable in Settings/Data Log (default 500ms)
 *           - Post-Correction card hides fields when disabled
 *           - Version format: single digit patch (v4.1.4 not v4.1.04)
 *   v4.1.3  - Wake-up detection accuracy & UI consistency:
 *           - Early GPIO snapshot: reads button pins immediately at boot (< 1ms)
 *           - Captures pin state before reed switch opens, solving misidentification
 *           - Volume-based post-correction with configurable thresholds (Settings/Data Log)
 *           - Post-correction events logged to btn_log.txt
 *           - CONFIG_VERSION 10: new DatalogConfig fields
 *           - UI: consistent save buttons, form-hints, page-headers, Moveâ†’Apply
 *   v4.1.2  - Wake-up fix:
 *           - Reverted to debounce-based button detection (esp_sleep_get_gpio_wakeup_status unreliable on ESP32-C3)
 *           - Reads all button states after debounce delay, FF has priority
 *           - CSS moved to external /style.css file (~30KB flash saved)
 *   v4.1.1  - UI & optimization:
 *           - File Manager: Move/Rename popup with folder selection
 *           - Settings hints: Hardware, Theme, Datalog descriptions
 *           - Restart popup unified (Hardware & Network use same helper)
 *           - Code size reduced (~5KB saved)
 *   v4.1.0  - Reliability improvements:
 *           - Wake-up uses esp_sleep_get_gpio_wakeup_status() for accurate button ID
 *           - Button debounce configurable in Settings > Hardware (20-500ms, default 100ms)
 *           - Edge detection for buttons - prevents multiple counts from held buttons
 *           - Non-blocking WiFi connect (doesn't delay button detection at startup)
 *           - Web handlers block during active measurement to prevent interference
 *           - Normal mode: auto-sleep after 2s if no button pressed at boot
 *   v4.0.0  - Major release:
 *           - Removed multi-language (English only, saves ~5KB flash)
 *           - Removed IconConfig from struct (config reset on upgrade)
 *           - System files always from LittleFS (logo, board, favicon, chart)
 *           - Dashboard: "Buttons Only" filter, "Exclude 0.00L" checkbox
 *           - Dashboard: Export filtered data to CSV
 *           - Settings: Import/Export with progress bars
 *           - Settings: Manual Device ID editing
 *           - File Manager: Upload progress bar
 *           - Datalog includes wake time AND sleep time
 *           - Restart button with confirmation popup
 *           - Test Mode: WiFi pin as LED output for visual check
 *   v3.6.14 - OTA restart fix, full CSS restored, storage view sync with config
 *   v3.6.12 - Changelog from LittleFS, About card, DEBUG_MODE macro
 *   v3.6.10 - ISR-safe pulseCount, atomic operations, JS translations
 * 
 * AUTHOR: Petko Georgiev
 **************************************************************************************************/

// Version constants
#define VERSION_MAJOR 4
#define VERSION_MINOR 1
#define VERSION_PATCH 4

// Debug mode - set to 1 to enable Serial debug output, 0 to save ~3KB flash
#define DEBUG_MODE 0

#if DEBUG_MODE
  #define DBG(x) Serial.print(x)
  #define DBGLN(x) Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

#define CONFIG_FREERTOS_UNICORE 1

#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <pgmspace.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <FlowSensor.h>
#include <Update.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <ArduinoJson.h>
#include <time.h>
#include <functional>
#include <vector>
#include "esp_mac.h"

// ============================================================================
// CONSTANTS
// ============================================================================
constexpr const char* CONFIG_FILE = "/config.bin";
constexpr const char* BOOTCOUNT_BACKUP_FILE = "/bootcount.bin";

constexpr const char* DEFAULT_AP_SSID = "WaterLogger";
constexpr const char* DEFAULT_AP_PASSWORD = "water12345";
constexpr const char* DEFAULT_DATALOG_PREFIX = "datalog";
constexpr const char* DEFAULT_NTP_SERVER = "pool.ntp.org";

const unsigned long TEST_MODE_BLINK_MS = 250;
const unsigned long TEST_MODE_HOLD_MS = 1000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const int LOG_BATCH_SIZE = 20;

// ISR Debounce for flow sensor (buttons use config.hardware.debounceMs)
const unsigned long ISR_DEBOUNCE_MICROS = 1000;  // 1ms for flow sensor

#define CONFIG_STRUCT_MAGIC 0xC0FFEE35
#define CONFIG_VERSION 10

// DS1302 RAM addresses for bootcount
#define RTC_RAM_BOOTCOUNT_ADDR 0
#define RTC_RAM_MAGIC_ADDR 4
#define RTC_RAM_MAGIC_VALUE 0xBC

// ============================================================================
// DEFAULT PIN DEFINITIONS FOR XIAO ESP32-C3
// ============================================================================
namespace DefaultPins {
    constexpr uint8_t WIFI_TRIGGER = 2;   // D0 on XIAO
    constexpr uint8_t WAKEUP_FF    = 3;   // D1 on XIAO
    constexpr uint8_t WAKEUP_PF    = 4;   // D2 on XIAO
    constexpr uint8_t FLOW_SENSOR  = 21;  // D6 on XIAO
    constexpr uint8_t RTC_CE       = 5;
    constexpr uint8_t RTC_IO       = 6;
    constexpr uint8_t RTC_SCLK     = 7;
    constexpr uint8_t SD_CS        = 10;
    constexpr uint8_t SD_MOSI      = 11;
    constexpr uint8_t SD_MISO      = 12;
    constexpr uint8_t SD_SCK       = 13;
}

// ============================================================================
// ENUMERATIONS
// ============================================================================
enum StorageType : uint8_t {
    STORAGE_LITTLEFS = 0,
    STORAGE_SD_CARD = 1
};

enum WiFiModeType : uint8_t {
    WIFIMODE_AP = 0,
    WIFIMODE_CLIENT = 1
};

enum ThemeMode : uint8_t {
    THEME_LIGHT = 0,
    THEME_DARK = 1,
    THEME_AUTO = 2
};

enum ChartSource : uint8_t {
    CHART_LOCAL = 0,
    CHART_CDN = 1
};

enum DatalogRotation : uint8_t {
    ROTATION_NONE = 0,
    ROTATION_DAILY = 1,
    ROTATION_WEEKLY = 2,
    ROTATION_MONTHLY = 3,
    ROTATION_SIZE = 4
};

enum WakeupMode : uint8_t {
    WAKEUP_GPIO_ACTIVE_HIGH = 0,
    WAKEUP_GPIO_ACTIVE_LOW = 1
};

// ============================================================================
// CONFIGURATION STRUCTURES
// ============================================================================
#pragma pack(push, 1)

// Chart column label format for Dashboard
enum ChartLabelFormat : uint8_t { 
    LABEL_DATETIME = 0,     // Date + Time (default)
    LABEL_BOOTCOUNT = 1,    // Boot count only (#1234)
    LABEL_BOTH = 2          // Both: Date Time #Boot
};

struct ThemeConfig {
    ThemeMode mode;
    char primaryColor[8];
    char secondaryColor[8];
    char accentColor[8];
    char bgColor[8];
    char textColor[8];
    char ffColor[8];
    char pfColor[8];
    char otherColor[8];
    char storageBarColor[8];
    char storageBar70Color[8];
    char storageBar90Color[8];
    char storageBarBorder[8];
    char logoSource[129];
    char faviconPath[33];        // Favicon path
    char boardDiagramPath[65];
    ChartSource chartSource;
    char chartLocalPath[65];
    bool showIcons;              // Show emoji icons in UI
    ChartLabelFormat chartLabelFormat;  // Dashboard column labels
};

// Datalog format options (Ð±Ð¸Ñ‚Ð¾Ð²Ð¸ Ñ„Ð»Ð°Ð³Ð¾Ð²Ðµ Ð·Ð° ÐºÐ¾Ð¼Ð¿Ð°ÐºÑ‚Ð½Ð¾ÑÑ‚)
enum DateFormat : uint8_t { DATE_OFF=0, DATE_DDMMYYYY=1, DATE_MMDDYYYY=2, DATE_YYYYMMDD=3, DATE_DDMMYYYY_DOT=4 };
enum TimeFormat : uint8_t { TIME_HHMMSS=0, TIME_HHMM=1, TIME_12H=2 };
enum EndFormat : uint8_t { END_TIME=0, END_DURATION=1, END_OFF=2 };
enum VolumeFormat : uint8_t { VOL_L_COMMA=0, VOL_L_DOT=1, VOL_NUM_ONLY=2, VOL_OFF=3 };

struct DatalogConfig {
    char prefix[33];
    char currentFile[65];
    char folder[33];
    DatalogRotation rotation;
    uint32_t maxSizeKB;
    uint16_t maxEntries;
    bool includeDeviceId;
    bool timestampFilename;
    // Format options (Ð¸Ð·Ð¿Ð¾Ð»Ð·Ð²Ð°Ð¼Ðµ reserved Ð±Ð°Ð¹Ñ‚Ð¾Ð²Ðµ)
    uint8_t dateFormat;      // DateFormat enum
    uint8_t timeFormat;      // TimeFormat enum
    uint8_t endFormat;       // EndFormat enum
    uint8_t volumeFormat;    // VolumeFormat enum
    bool includeBootCount;
    bool includeExtraPresses;
    // Post-correction settings (v4.1.4+)
    bool postCorrectionEnabled;  // Enable/disable volume-based button ID correction
    float pfToFfThreshold;       // PF_BTN measured above this -> corrected to FF_BTN (default 4.5L)
    float ffToPfThreshold;       // FF_BTN measured below this -> corrected to PF_BTN (default 3.7L)
    uint16_t manualPressThresholdMs;  // Button held longer than this = manual press, skip correction (default 500ms)
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
    uint16_t debounceMs;        // Button debounce time in milliseconds
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
    // AP network settings
    uint8_t apIP[4];
    uint8_t apGateway[4];
    uint8_t apSubnet[4];
    uint8_t reserved[2];
    String deviceId;
};

struct DeviceConfig {
    uint32_t magic;
    uint8_t version;
    char deviceId[13];
    char deviceName[33];
    uint8_t _reserved_lang;      // Was: Language (kept for struct size)
    bool forceWebServer;
    int8_t resetBootCountAction;
    ThemeConfig theme;
    DatalogConfig datalog;
    FlowMeterConfig flowMeter;
    HardwareConfig hardware;
    NetworkConfig network;
};

struct LogEntry {
    uint32_t wakeTimestamp;   // Time when device woke up
    uint32_t sleepTimestamp;  // Time when device goes to sleep
    uint16_t bootCount;
    uint16_t ffCount;
    uint16_t pfCount;
    float volumeLiters;
    char wakeupReason[10];
};

#pragma pack(pop)

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
DeviceConfig config;

ThreeWire* rtcWire = nullptr;
RtcDS1302<ThreeWire>* Rtc = nullptr;
FlowSensor* Sensor = nullptr;
AsyncWebServer server(80);

fs::FS* activeFS = nullptr;        // Active filesystem for data logging
bool sdAvailable = false;           // SD card mounted
bool littleFsAvailable = false;     // LittleFS mounted
String currentStorageView = "internal";  // Which storage File Manager shows

RTC_DATA_ATTR LogEntry logBuffer[LOG_BATCH_SIZE];
RTC_DATA_ATTR int logBufferCount = 0;
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool bootcount_restore = true;

// Wake timestamp - captured when device wakes up
uint32_t currentWakeTimestamp = 0;

// ISR variables with proper volatile
volatile uint32_t pulseCount = 0;
volatile unsigned long lastFFInterrupt = 0;
volatile unsigned long lastPFInterrupt = 0;
volatile unsigned long lastFlowInterrupt = 0;
volatile bool ffPressed = false;
volatile bool pfPressed = false;
volatile bool flowSensorPulseDetected = false;
volatile unsigned long isrDebounceUs = 100000;  // ISR debounce in microseconds (updated from config)

// Counting and debounce variables
int highCountFF = 0;
int highCountPF = 0;
int stableFFState = LOW, stablePFState = LOW;
unsigned long lastFFDebounceTime = 0, lastPFDebounceTime = 0;
int lastFFButtonState = LOW, lastPFButtonState = LOW;

unsigned long lastLoggingCycleStartTime = 0;
unsigned long testModeLastPulseTime = 0;
bool fsAvailable = false;
String wakeUpButtonStr = "";
String statusMessage = "";
String currentDir = "/";
bool apModeTriggered = false;
bool wifiConnectedAsClient = false;
bool rtcValid = false;
bool wifiFallbackToAP = false;
String currentIPAddress = "";
String connectedSSID = "";
bool shouldRestart = false;
unsigned long restartTimer = 0;

// Online Logger Mode (Force Web Server with logging)
bool onlineLoggerMode = false;
String cycleStartedBy = "BOOT";  // "FF_BTN", "PF_BTN", "PWR_ON", "IDLE"

// Early GPIO snapshot - captured at boot BEFORE config/storage/hardware init
// Stores raw digitalRead values for ALL potentially relevant GPIOs.
// Interpreted later by getWakeupReason() using actual config pin numbers.
// This captures pin state within ~1ms of wake-up, before reed switch opens.
uint32_t earlyGPIO_bitmask = 0;  // Bit N = digitalRead(GPIO N) at boot
bool earlyGPIO_captured = false;
unsigned long earlyGPIO_millis = 0;  // millis() at time of early GPIO capture
unsigned long buttonHeldMs = 0;      // How long the flush plate button was held down
unsigned long cycleStartTime = 0;
uint32_t cycleTotalPulses = 0;
bool cycleButtonSet = false;

// Logging State Machine
enum LoggingState {
    STATE_IDLE,           // Waiting for button press
    STATE_WAIT_FLOW,      // Button pressed, waiting for flow (6 sec window)
    STATE_MONITORING,     // Flow detected, monitoring (3 sec after last pulse)
    STATE_DONE            // Ready to log and sleep/reset
};
LoggingState loggingState = STATE_IDLE;
unsigned long stateStartTime = 0;
unsigned long lastFlowPulseTime = 0;

// Timing constants (in milliseconds)
const unsigned long BUTTON_WAIT_FLOW_MS = 6000;   // 6 sec window to wait for flow after button
const unsigned long FLOW_IDLE_TIMEOUT_MS = 3000;  // 3 sec after last pulse before logging

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void loadDefaultConfig();
bool loadConfig();
bool saveConfig();
void migrateConfig(uint8_t fromVersion);
void setupWebServer();
void initHardware();
bool initStorage();
bool connectToWiFi();
void startAPMode();
void safeWiFiShutdown();
void configureWakeup();
String getActiveDatalogFile();
String buildPath(const String &dir, const String &name);
String sanitizePath(const String &path);
String sanitizeFilename(const String& filename);
bool deleteRecursive(fs::FS &fs, const String &path);
int countDatalogFiles();
void backupBootCount();
void restoreBootCount();
bool syncTimeFromNTP();
void sendJsonResponse(AsyncWebServerRequest *r, JsonDocument &doc);
void sendRestartPage(AsyncWebServerRequest *r, const char* message);
void sendChunkedHtml(AsyncWebServerRequest *r, const char* title, std::function<void(Print&)> bodyWriter);
void addLogEntry();
void flushLogBufferToFS();
void debounceButton(uint8_t pin, int &last, int &stable, unsigned long &lastTime, int &count);

// Forward declarations for utility functions used in templates
String getRtcTimeString();
String getRtcDateTimeString();
String getNetworkDisplay();

// ISR handlers
void IRAM_ATTR onFFButton();
void IRAM_ATTR onPFButton();
void IRAM_ATTR onFlowPulse();

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================
String formatFileSize(uint64_t bytes) {
    if (bytes >= 1073741824) return String(bytes / 1073741824.0, 2) + " GB";
    if (bytes >= 1048576) return String(bytes / 1048576.0, 1) + " MB";
    if (bytes >= 1024) return String(bytes / 1024.0, 1) + " KB";
    return String((unsigned long)bytes) + " B";
}

// Get firmware version string
String getVersionString() {
    char buf[16];
    snprintf(buf, sizeof(buf), "v%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    return String(buf);
}

// Get current mode display
String getModeDisplay() {
    if (onlineLoggerMode) return "ðŸŸ¢ Online Logger";
    if (apModeTriggered) return "ðŸ”µ Web Server";
    return "ðŸ”´ Logger";
}

// Helper to get icon - returns custom image or emoji
// Simple icon helper - returns emoji if showIcons enabled
String icon(const char* emoji) {
    return config.theme.showIcons ? String(emoji) + " " : "";
}

String getThemeClass() {
    switch (config.theme.mode) {
        case THEME_DARK: return "theme-dark";
        case THEME_AUTO: return "theme-auto";
        default: return "theme-light";
    }
}

String getNetworkDisplay() {
    if (wifiConnectedAsClient) return "ðŸ“¶ " + connectedSSID;
    return "ðŸ“¡ AP: " + String(strlen(config.network.apSSID) > 0 ? config.network.apSSID : config.deviceName);
}

// Write consistent status bar header for all pages
String generateDeviceId() {
    uint8_t mac[6];
    // Use built-in chip MAC address which is always available
    esp_read_mac(mac, ESP_MAC_WIFI_STA); 
    
    char id[13];
    // Use last 4 bytes for short ID
    snprintf(id, sizeof(id), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    return String(id);
}

void regenerateDeviceId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); // Read unique chip address
    
    char id[13];
    // Generate ID based on last 4 bytes of MAC address
    snprintf(id, sizeof(id), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    
    config.network.deviceId = String(id);
    saveConfig(); // Save immediately to config.bin
    DBG("New Device ID generated: ");
    DBGLN(config.network.deviceId);
}

String getActiveDatalogFile() {
    if (strlen(config.datalog.currentFile) > 0) {
        return String(config.datalog.currentFile);
    }
    String folder = String(config.datalog.folder);
    if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
    if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
    return folder + String(config.datalog.prefix) + "_datalog.txt";
}

String buildPath(const String &dir, const String &name) {
    if (dir == "/" || dir.isEmpty()) return "/" + name;
    return dir + "/" + name;
}

// Sanitize path - remove dangerous sequences
String sanitizePath(const String &path) {
    String safe = path;
    safe.replace("..", "");
    safe.replace("//", "/");
    if (!safe.startsWith("/")) safe = "/" + safe;
    while (safe.length() > 1 && safe.endsWith("/")) safe = safe.substring(0, safe.length() - 1);
    return safe;
}

String sanitizeFilename(const String& filename) {
    String safe = filename;
    safe.replace("..", "");
    safe.replace("//", "/");
    while (safe.indexOf("//") >= 0) safe.replace("//", "/");
    return safe;
}

// ============================================================================
// ISR HANDLERS WITH DEBOUNCE
// ============================================================================
void IRAM_ATTR onFFButton() {
    unsigned long now = micros();
    if (now - lastFFInterrupt > isrDebounceUs) {
        lastFFInterrupt = now;
        ffPressed = true;
    }
}

void IRAM_ATTR onPFButton() {
    unsigned long now = micros();
    if (now - lastPFInterrupt > isrDebounceUs) {
        lastPFInterrupt = now;
        pfPressed = true;
    }
}

void IRAM_ATTR onFlowPulse() {
    unsigned long now = micros();
    if (now - lastFlowInterrupt > ISR_DEBOUNCE_MICROS) {
        if (Sensor) Sensor->count();
        pulseCount++;
        lastFlowInterrupt = now;
        flowSensorPulseDetected = true;
    }
}

// Polling-based debounce for buttons (alternative to ISR)
void debounceButton(uint8_t pin, int &last, int &stable, unsigned long &lastTime, int &count) {
    int reading = digitalRead(pin);
    if (reading != last) { 
        lastTime = millis(); 
        last = reading; 
    }
    if ((millis() - lastTime) > config.hardware.debounceMs && reading != stable) {
        int previousStable = stable;
        stable = reading;
        int expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
        int expectedInactive = (expectedActive == HIGH) ? LOW : HIGH;
        // Count only on transition from RELEASED to PRESSED (edge detection)
        // This prevents counting held buttons multiple times
        if (previousStable == expectedInactive && stable == expectedActive) {
            count++;
        }
    }
}

// ============================================================================
// NTP TIME SYNC
// ============================================================================
bool syncTimeFromNTP() {
    if (!wifiConnectedAsClient) {
        DBGLN("NTP: No WiFi connection");
        return false;
    }
    
    DBGF("NTP: Syncing from %s...\n", config.network.ntpServer);
    
    configTime(config.network.timezone * 3600, 0, config.network.ntpServer);
    
    // Wait for time to be set (max 10 seconds)
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    while (timeinfo.tm_year < (2020 - 1900) && retry < 20) {
        delay(500);
        time(&now);
        localtime_r(&now, &timeinfo);
        retry++;
    }
    
    if (timeinfo.tm_year < (2020 - 1900)) {
        DBGLN("NTP: Failed to get time");
        return false;
    }
    
    // Set RTC
    if (Rtc) {
        // Ensure RTC is writable
        Rtc->SetIsWriteProtected(false);
        Rtc->SetIsRunning(true);
        
        RtcDateTime dt(
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        );
        Rtc->SetDateTime(dt);
        DBGF("NTP: RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
            dt.Year(), dt.Month(), dt.Day(),
            dt.Hour(), dt.Minute(), dt.Second());
    }
    
    return true;
}

// ============================================================================
// BOOTCOUNT BACKUP/RESTORE (RTC RAM + Flash)
// ============================================================================
void backupBootCount() {
    if (Rtc) {
        Rtc->SetMemory((uint8_t)RTC_RAM_MAGIC_ADDR, (uint8_t)RTC_RAM_MAGIC_VALUE);
        Rtc->SetMemory((uint8_t)RTC_RAM_BOOTCOUNT_ADDR, (uint8_t)((bootCount >> 24) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 1), (uint8_t)((bootCount >> 16) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 2), (uint8_t)((bootCount >> 8) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 3), (uint8_t)(bootCount & 0xFF));
    }
    File f = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "w");
    if (f) { f.write((uint8_t*)&bootCount, sizeof(bootCount)); f.close(); }
}

void restoreBootCount() {
    if (Rtc) {
        uint8_t magic = Rtc->GetMemory((uint8_t)RTC_RAM_MAGIC_ADDR);
        if (magic == RTC_RAM_MAGIC_VALUE) {
            bootCount = ((uint32_t)Rtc->GetMemory((uint8_t)RTC_RAM_BOOTCOUNT_ADDR) << 24) |
                        ((uint32_t)Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 1)) << 16) |
                        ((uint32_t)Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 2)) << 8) |
                        Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 3));
            DBGF("Bootcount from RTC RAM: %d\n", bootCount);
            return;
        }
    }
    File f = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "r");
    if (f) { 
        f.read((uint8_t*)&bootCount, sizeof(bootCount)); 
        f.close();
        DBGF("Bootcount from flash: %d\n", bootCount);
    }
}

// ============================================================================
// DATA LOGGING
// ============================================================================
void flushLogBufferToFS() {
    if (logBufferCount == 0 || !fsAvailable || !activeFS) return;
    
    String logFile = getActiveDatalogFile();
    
    // Create folder if needed
    if (strlen(config.datalog.folder) > 0) {
        String folder = String(config.datalog.folder);
        if (!folder.startsWith("/")) folder = "/" + folder;
        if (!activeFS->exists(folder)) {
            activeFS->mkdir(folder);
        }
    }
    
    File f = activeFS->open(logFile, FILE_APPEND);
    if (!f) {
        Serial.println("ERR: Can't open datalog");
        return;
    }
    
    for (int i = 0; i < logBufferCount; i++) {
        RtcDateTime wakeTime, sleepTime;
        wakeTime.InitWithUnix32Time(logBuffer[i].wakeTimestamp);
        sleepTime.InitWithUnix32Time(logBuffer[i].sleepTimestamp);
        
        String line = "";
        
        // Date format
        if (config.datalog.dateFormat != DATE_OFF) {
            char dateBuf[12];
            switch (config.datalog.dateFormat) {
                case DATE_DDMMYYYY:     snprintf(dateBuf, 12, "%02u/%02u/%04u", wakeTime.Day(), wakeTime.Month(), wakeTime.Year()); break;
                case DATE_MMDDYYYY:     snprintf(dateBuf, 12, "%02u/%02u/%04u", wakeTime.Month(), wakeTime.Day(), wakeTime.Year()); break;
                case DATE_YYYYMMDD:     snprintf(dateBuf, 12, "%04u-%02u-%02u", wakeTime.Year(), wakeTime.Month(), wakeTime.Day()); break;
                case DATE_DDMMYYYY_DOT: snprintf(dateBuf, 12, "%02u.%02u.%04u", wakeTime.Day(), wakeTime.Month(), wakeTime.Year()); break;
                default: dateBuf[0] = 0;
            }
            line += dateBuf;
        }
        
        // Start time format
        char timeBuf[12];
        switch (config.datalog.timeFormat) {
            case TIME_HHMMSS: snprintf(timeBuf, 12, "%02u:%02u:%02u", wakeTime.Hour(), wakeTime.Minute(), wakeTime.Second()); break;
            case TIME_HHMM:   snprintf(timeBuf, 12, "%02u:%02u", wakeTime.Hour(), wakeTime.Minute()); break;
            case TIME_12H: {
                uint8_t h = wakeTime.Hour() % 12; if (h == 0) h = 12;
                snprintf(timeBuf, 12, "%u:%02u:%02u%s", h, wakeTime.Minute(), wakeTime.Second(), wakeTime.Hour() < 12 ? "AM" : "PM");
                break;
            }
        }
        if (line.length() > 0) line += "|";
        line += timeBuf;
        
        // End format (time, duration, or off)
        if (config.datalog.endFormat != END_OFF) {
            line += "|";
            if (config.datalog.endFormat == END_TIME) {
                switch (config.datalog.timeFormat) {
                    case TIME_HHMMSS: snprintf(timeBuf, 12, "%02u:%02u:%02u", sleepTime.Hour(), sleepTime.Minute(), sleepTime.Second()); break;
                    case TIME_HHMM:   snprintf(timeBuf, 12, "%02u:%02u", sleepTime.Hour(), sleepTime.Minute()); break;
                    case TIME_12H: {
                        uint8_t h = sleepTime.Hour() % 12; if (h == 0) h = 12;
                        snprintf(timeBuf, 12, "%u:%02u:%02u%s", h, sleepTime.Minute(), sleepTime.Second(), sleepTime.Hour() < 12 ? "AM" : "PM");
                        break;
                    }
                }
                line += timeBuf;
            } else { // END_DURATION
                uint32_t dur = logBuffer[i].sleepTimestamp - logBuffer[i].wakeTimestamp;
                line += String(dur) + "s";
            }
        }
        
        // Boot count
        if (config.datalog.includeBootCount) {
            line += "|#:";
            line += String(logBuffer[i].bootCount);
        }
        
        // Trigger (always included)
        line += "|";
        line += logBuffer[i].wakeupReason;
        
        // Volume format
        if (config.datalog.volumeFormat != VOL_OFF) {
            line += "|";
            String volStr = String(logBuffer[i].volumeLiters, 2);
            if (config.datalog.volumeFormat == VOL_L_COMMA) {
                volStr.replace('.', ',');
                line += "L:" + volStr;
            } else if (config.datalog.volumeFormat == VOL_L_DOT) {
                line += "L:" + volStr;
            } else { // VOL_NUM_ONLY
                line += volStr;
            }
        }
        
        // Extra presses
        if (config.datalog.includeExtraPresses) {
            line += "|FF" + String(logBuffer[i].ffCount);
            line += "|PF" + String(logBuffer[i].pfCount);
        }
        
        f.println(line);
    }
    
    f.close();
    int cnt = logBufferCount;
    logBufferCount = 0;
    backupBootCount();
    DBGF("Flushed %d entries to %s\n", cnt, logFile.c_str());
}

void addLogEntry() {
    if (logBufferCount >= LOG_BATCH_SIZE) {
        flushLogBufferToFS();
        if (logBufferCount >= LOG_BATCH_SIZE) {
            for (int i = 0; i < LOG_BATCH_SIZE - 1; i++) 
                logBuffer[i] = logBuffer[i + 1];
            logBufferCount = LOG_BATCH_SIZE - 1;
        }
    }
    
    int i = logBufferCount;
    
    // Wake timestamp - captured at boot or cycle start
    logBuffer[i].wakeTimestamp = currentWakeTimestamp;
    
    // Sleep timestamp - current time (when we're about to sleep)
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        logBuffer[i].sleepTimestamp = now.IsValid() ? now.Unix32Time() : 0;
    } else {
        logBuffer[i].sleepTimestamp = 0;
    }
    
    logBuffer[i].bootCount = bootCount;
    logBuffer[i].ffCount = highCountFF;
    logBuffer[i].pfCount = highCountPF;
    
    // Atomic read and reset of pulseCount (ISR-safe)
    noInterrupts();
    uint32_t safePulseCount = pulseCount;
    pulseCount = 0;
    interrupts();
    
    // Calculate volume from pulses
    logBuffer[i].volumeLiters = (float)safePulseCount / config.flowMeter.pulsesPerLiter * config.flowMeter.calibrationMultiplier;
    
    // Use cycleStartedBy for Online Logger mode (reflects actual trigger), wakeUpButtonStr for normal mode
    String reason = onlineLoggerMode ? cycleStartedBy : wakeUpButtonStr;
    strncpy(logBuffer[i].wakeupReason, reason.c_str(), 9);
    logBuffer[i].wakeupReason[9] = '\0';
    
    logBufferCount++;
    highCountFF = 0;
    highCountPF = 0;
}

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================
void loadDefaultConfig() {
    memset(&config, 0, sizeof(DeviceConfig));
    
    config.magic = CONFIG_STRUCT_MAGIC;
    config.version = CONFIG_VERSION;
    
    String devId = generateDeviceId();
    strncpy(config.deviceId, devId.c_str(), sizeof(config.deviceId) - 1);
    strncpy(config.deviceName, "Water Logger", sizeof(config.deviceName) - 1);
    config.forceWebServer = false;
    config.resetBootCountAction = -1;
    
    // Theme defaults
    config.theme.mode = THEME_AUTO;
    strcpy(config.theme.primaryColor, "#275673");
    strcpy(config.theme.secondaryColor, "#4a5568");
    strcpy(config.theme.accentColor, "#3182ce");
    strcpy(config.theme.bgColor, "#f7fafc");
    strcpy(config.theme.textColor, "#2d3748");
    strcpy(config.theme.ffColor, "#275673");
    strcpy(config.theme.pfColor, "#7eb0d5");
    strcpy(config.theme.otherColor, "#a0aec0");
    strcpy(config.theme.storageBarColor, "#27ae60");
    strcpy(config.theme.storageBar70Color, "#f39c12");
    strcpy(config.theme.storageBar90Color, "#e74c3c");
    strcpy(config.theme.storageBarBorder, "#cccccc");
    config.theme.chartSource = CHART_CDN;
    strcpy(config.theme.chartLocalPath, "/chart.min.js");
    config.theme.showIcons = true;  // Show emoji icons by default
    config.theme.chartLabelFormat = LABEL_DATETIME;  // Date+Time by default
    
    // Datalog defaults
    strcpy(config.datalog.prefix, DEFAULT_DATALOG_PREFIX);
    strcpy(config.datalog.currentFile, "/datalog.txt");
    strcpy(config.datalog.folder, "");
    config.datalog.rotation = ROTATION_NONE;
    config.datalog.maxSizeKB = 1024;
    config.datalog.maxEntries = 10000;
    config.datalog.includeDeviceId = false;
    config.datalog.timestampFilename = true;
    // Format defaults
    config.datalog.dateFormat = DATE_DDMMYYYY;
    config.datalog.timeFormat = TIME_HHMMSS;
    config.datalog.endFormat = END_TIME;
    config.datalog.volumeFormat = VOL_L_COMMA;
    config.datalog.includeBootCount = true;
    config.datalog.includeExtraPresses = true;
    // Post-correction defaults
    config.datalog.postCorrectionEnabled = true;
    config.datalog.pfToFfThreshold = 4.5f;
    config.datalog.ffToPfThreshold = 3.7f;
    config.datalog.manualPressThresholdMs = 500;
    
    // Flow meter defaults
    config.flowMeter.pulsesPerLiter = 450.0f;
    config.flowMeter.calibrationMultiplier = 1.0f;
    config.flowMeter.monitoringWindowSecs = 3;
    config.flowMeter.firstLoopMonitoringWindowSecs = 6;
    config.flowMeter.testMode = false;
    config.flowMeter.blinkDuration = 250;
    
    // Hardware defaults
    config.hardware.version = CONFIG_VERSION;
    config.hardware.storageType = STORAGE_LITTLEFS;
    config.hardware.wakeupMode = WAKEUP_GPIO_ACTIVE_HIGH;
    config.hardware.pinWifiTrigger = DefaultPins::WIFI_TRIGGER;
    config.hardware.pinWakeupFF = DefaultPins::WAKEUP_FF;
    config.hardware.pinWakeupPF = DefaultPins::WAKEUP_PF;
    config.hardware.pinFlowSensor = DefaultPins::FLOW_SENSOR;
    config.hardware.pinRtcCE = DefaultPins::RTC_CE;
    config.hardware.pinRtcIO = DefaultPins::RTC_IO;
    config.hardware.pinRtcSCLK = DefaultPins::RTC_SCLK;
    config.hardware.pinSdCS = DefaultPins::SD_CS;
    config.hardware.pinSdMOSI = DefaultPins::SD_MOSI;
    config.hardware.pinSdMISO = DefaultPins::SD_MISO;
    config.hardware.pinSdSCK = DefaultPins::SD_SCK;
    config.hardware.cpuFreqMHz = 80;
    config.hardware.debugMode = false;
    config.hardware.defaultStorageView = 0;
    config.hardware.debounceMs = 100;  // 100ms default debounce
    
    // Network defaults
    config.network.wifiMode = WIFIMODE_AP;
    strcpy(config.network.apPassword, DEFAULT_AP_PASSWORD);
    strcpy(config.network.ntpServer, DEFAULT_NTP_SERVER);
    config.network.timezone = 2;  // EET (Bulgaria)
    config.network.useStaticIP = false;
    // Client mode static IP defaults
    config.network.staticIP[0] = 192; config.network.staticIP[1] = 168;
    config.network.staticIP[2] = 4; config.network.staticIP[3] = 100;
    config.network.gateway[0] = 192; config.network.gateway[1] = 168;
    config.network.gateway[2] = 4; config.network.gateway[3] = 1;
    config.network.subnet[0] = 255; config.network.subnet[1] = 255;
    config.network.subnet[2] = 255; config.network.subnet[3] = 0;
    config.network.dns[0] = 8; config.network.dns[1] = 8;
    config.network.dns[2] = 8; config.network.dns[3] = 8;
    // AP mode IP defaults
    config.network.apIP[0] = 192; config.network.apIP[1] = 168;
    config.network.apIP[2] = 4; config.network.apIP[3] = 1;
    config.network.apGateway[0] = 192; config.network.apGateway[1] = 168;
    config.network.apGateway[2] = 4; config.network.apGateway[3] = 1;
    config.network.apSubnet[0] = 255; config.network.apSubnet[1] = 255;
    config.network.apSubnet[2] = 255; config.network.apSubnet[3] = 0;
}

void migrateConfig(uint8_t fromVersion) {
    DBGF("Migrating config from v%d to v%d\n", fromVersion, CONFIG_VERSION);
    
    // Migration from v5 to v6: Add NTP settings
    if (fromVersion < 6) {
        strcpy(config.network.ntpServer, DEFAULT_NTP_SERVER);
        config.network.timezone = 2;
    }
    
    // Migration from v9 to v10: Add post-correction settings
    if (fromVersion < 10) {
        config.datalog.postCorrectionEnabled = true;
        config.datalog.pfToFfThreshold = 4.5f;
        config.datalog.ffToPfThreshold = 3.7f;
        config.datalog.manualPressThresholdMs = 500;
    }
    
    // Update version
    config.version = CONFIG_VERSION;
    saveConfig();
}

bool loadConfig() {
    if (!LittleFS.begin(true)) {
        DBGLN("LittleFS mount failed");
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
    
    // If file matches current struct size, read directly
    if (fileSize == sizeof(DeviceConfig)) {
        size_t readBytes = f.read((uint8_t*)&config, sizeof(DeviceConfig));
        f.close();
        
        if (readBytes != sizeof(DeviceConfig) || config.magic != CONFIG_STRUCT_MAGIC) {
            DBGLN("Invalid config, loading defaults");
            loadDefaultConfig();
            return false;
        }
    } else if (fileSize > 0 && fileSize < sizeof(DeviceConfig)) {
        // Older/smaller config file - need careful migration
        // Read raw bytes into a temporary buffer
        uint8_t* rawBuf = (uint8_t*)malloc(fileSize);
        if (!rawBuf) {
            f.close();
            DBGLN("Config migration: malloc failed");
            loadDefaultConfig();
            return false;
        }
        
        size_t readBytes = f.read(rawBuf, fileSize);
        f.close();
        
        // Check magic from raw buffer
        uint32_t fileMagic;
        memcpy(&fileMagic, rawBuf, sizeof(uint32_t));
        if (readBytes != fileSize || fileMagic != CONFIG_STRUCT_MAGIC) {
            free(rawBuf);
            DBGF("Invalid config magic in migration (read %d of %d bytes)\n", readBytes, fileSize);
            loadDefaultConfig();
            return false;
        }
        
        // Start with clean defaults, then overlay old data
        loadDefaultConfig();
        
        // Calculate where DatalogConfig ends in old vs new struct
        // Old struct had DatalogConfig with reserved[6] (6 bytes)
        // New struct has postCorrectionEnabled(1) + pfToFfThreshold(4) + ffToPfThreshold(4) + reserved[1] (1)
        // The offset of datalog within DeviceConfig is the same for both versions
        size_t datalogOffset = offsetof(DeviceConfig, datalog);
        
        // Copy everything BEFORE DatalogConfig's new fields (this is identical layout)
        // = datalogOffset + old DatalogConfig fields up to and including includeExtraPresses
        // Old DatalogConfig size = fileSize - (everything before datalog) - (everything after datalog in old)
        // Simpler: copy the header + theme + old datalog portion
        size_t oldDatalogSize = fileSize - datalogOffset - (fileSize - datalogOffset - (sizeof(DatalogConfig) - 9)); // complex...
        
        // Actually, simplest correct approach: copy fields group by group
        // 1. Copy header fields (magic through resetBootCountAction) - same offset
        size_t headerSize = offsetof(DeviceConfig, theme);
        memcpy(&config, rawBuf, headerSize);
        
        // 2. Copy ThemeConfig - same offset
        memcpy(&config.theme, rawBuf + offsetof(DeviceConfig, theme), sizeof(ThemeConfig));
        
        // 3. Copy old DatalogConfig fields (without new post-correction fields)
        // Old datalog had: prefix[33]+currentFile[65]+folder[33]+rotation(4)+maxSizeKB(4)+maxEntries(2)
        //   +includeDeviceId(1)+timestampFilename(1)+dateFormat(1)+timeFormat(1)+endFormat(1)+volumeFormat(1)
        //   +includeBootCount(1)+includeExtraPresses(1)+reserved[6] = total varies by padding
        // Safest: copy sizeof(DatalogConfig) - 9 bytes (the part before new fields)
        // The common prefix is everything up to includeExtraPresses
        size_t commonDatalogSize = offsetof(DatalogConfig, postCorrectionEnabled);
        memcpy(&config.datalog, rawBuf + datalogOffset, commonDatalogSize);
        // New fields keep their defaults from loadDefaultConfig()
        
        // 4. Calculate old DatalogConfig total size (with reserved[6])
        // = commonDatalogSize + 6 bytes reserved = commonDatalogSize + 6
        // But we need to account for padding. Let's compute it from file size difference.
        size_t sizeDiff = sizeof(DeviceConfig) - fileSize;  // how much the struct grew
        size_t oldDatalogTotalSize = sizeof(DatalogConfig) - sizeDiff;
        
        // 5. Copy FlowMeterConfig from old offset
        size_t oldFlowMeterOffset = datalogOffset + oldDatalogTotalSize;
        if (oldFlowMeterOffset + sizeof(FlowMeterConfig) <= fileSize) {
            memcpy(&config.flowMeter, rawBuf + oldFlowMeterOffset, sizeof(FlowMeterConfig));
        }
        
        // 6. Copy HardwareConfig from old offset
        size_t oldHardwareOffset = oldFlowMeterOffset + sizeof(FlowMeterConfig);
        if (oldHardwareOffset + sizeof(HardwareConfig) <= fileSize) {
            memcpy(&config.hardware, rawBuf + oldHardwareOffset, sizeof(HardwareConfig));
        }
        
        // 7. Copy NetworkConfig from old offset
        size_t oldNetworkOffset = oldHardwareOffset + sizeof(HardwareConfig);
        size_t networkBytesAvailable = (oldNetworkOffset < fileSize) ? (fileSize - oldNetworkOffset) : 0;
        if (networkBytesAvailable > 0) {
            size_t copySize = (networkBytesAvailable < sizeof(NetworkConfig)) ? networkBytesAvailable : sizeof(NetworkConfig);
            memcpy(&config.network, rawBuf + oldNetworkOffset, copySize);
        }
        
        free(rawBuf);
        
        DBGF("Config migrated from %d to %d bytes\n", fileSize, sizeof(DeviceConfig));
    } else {
        f.close();
        DBGF("Config file size mismatch (%d vs %d), loading defaults\n", fileSize, sizeof(DeviceConfig));
        loadDefaultConfig();
        return false;
    }
    
    // Check for version migration (sets new field defaults)
    if (config.version < CONFIG_VERSION) {
        migrateConfig(config.version);
    }
    
    DBGF("Config loaded, version %d\n", config.version);
    return true;
}

bool saveConfig() {
    config.magic = CONFIG_STRUCT_MAGIC;
    config.version = CONFIG_VERSION;
    
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        DBGLN("Failed to save config");
        return false;
    }
    
    f.write((uint8_t*)&config, sizeof(DeviceConfig));
    f.close();
    DBGLN("Config saved");
    return true;
}

// ============================================================================
// STORAGE & HARDWARE INITIALIZATION
// ============================================================================
bool initStorage() {
    // Always try to mount LittleFS first (for config, bootcount, etc.)
    DBGLN("Init LittleFS...");
    if (LittleFS.begin(true)) {
        DBGLN("LittleFS OK");
        littleFsAvailable = true;
    } else {
        DBGLN("LittleFS FAILED!");
        littleFsAvailable = false;
    }
    
    // Try to mount SD card if configured
    if (config.hardware.storageType == STORAGE_SD_CARD) {
        DBGLN("Init SD Card...");
        SPI.begin(config.hardware.pinSdSCK, config.hardware.pinSdMISO,
                  config.hardware.pinSdMOSI, config.hardware.pinSdCS);
        if (SD.begin(config.hardware.pinSdCS)) {
            DBGF("SD Card OK - Size: %llu MB\n", SD.cardSize() / (1024 * 1024));
            sdAvailable = true;
        } else {
            DBGLN("SD Card FAILED!");
            sdAvailable = false;
        }
    }
    
    // Set active filesystem for data logging
    if (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) {
        activeFS = &SD;
        fsAvailable = true;
        currentStorageView = "sdcard";
        DBGLN("Active FS: SD Card");
    } else if (littleFsAvailable) {
        activeFS = &LittleFS;
        fsAvailable = true;
        currentStorageView = "internal";
        DBGLN("Active FS: LittleFS");
    } else {
        activeFS = nullptr;
        fsAvailable = false;
        Serial.println("ERR: No storage available!");
        return false;
    }
    
    return true;
}

void initHardware() {
    DBGLN("Init hardware...");
    
    // Validate pin numbers before using them
    bool pinsValid = true;
    
    // Check for invalid/dangerous pins on ESP32-C3
    // GPIO 8, 9 are connected to flash on most ESP32-C3 boards
    // GPIO 18, 19 are USB on some boards
    auto isPinSafe = [](int pin) {
        if (pin < 0 || pin > 21) return false;
        // ESP32-C3 flash pins - avoid these
        if (pin == 11 || pin == 12 || pin == 13 || pin == 14 || pin == 15 || pin == 16 || pin == 17) return false;
        return true;
    };
    
    if (!isPinSafe(config.hardware.pinRtcCE) || 
        !isPinSafe(config.hardware.pinRtcIO) || 
        !isPinSafe(config.hardware.pinRtcSCLK)) {
        DBGLN("WARNING: RTC pins may be invalid!");
        pinsValid = false;
    }
    
    // Try to initialize RTC with failsafe
    bool rtcOk = false;
    if (pinsValid) {
        if (rtcWire) delete rtcWire;
        if (Rtc) delete Rtc;
        
        DBGF("RTC Pins: CE=%d, IO=%d, SCLK=%d\n", 
            config.hardware.pinRtcCE, config.hardware.pinRtcIO, config.hardware.pinRtcSCLK);
        
        rtcWire = new ThreeWire(config.hardware.pinRtcIO, config.hardware.pinRtcSCLK, config.hardware.pinRtcCE);
        Rtc = new RtcDS1302<ThreeWire>(*rtcWire);
        
        Rtc->Begin();
        
        // DS1302 specific: Remove write protection and start oscillator
        if (Rtc->GetIsWriteProtected()) {
            DBGLN("RTC: Removing write protection...");
            Rtc->SetIsWriteProtected(false);
        }
        
        if (!Rtc->GetIsRunning()) {
            DBGLN("RTC: Starting oscillator...");
            Rtc->SetIsRunning(true);
        }
        
        // Test if RTC responds with valid data
        RtcDateTime test = Rtc->GetDateTime();
        DBGF("RTC raw read: Year=%d, Month=%d, Day=%d, Hour=%d, Min=%d, Sec=%d\n",
            test.Year(), test.Month(), test.Day(), 
            test.Hour(), test.Minute(), test.Second());
        
        // Check for common error values
        // Year 2000 with Month=0 means RTC is at default/unset state
        // Year 165 (0xA5), 2165 often indicate communication failure
        if (test.Year() >= 2020 && test.Year() <= 2100 && 
            test.Month() >= 1 && test.Month() <= 12 &&
            test.Day() >= 1 && test.Day() <= 31) {
            rtcOk = true;
            DBGLN("RTC: Valid time detected");
        } else {
            DBGLN("RTC: Time not set or invalid");
            // Year 2000 Month 0 = RTC needs to be set (common after battery replacement)
            if (test.Year() == 2000 && test.Month() == 0) {
                DBGLN("RTC: Appears to be at factory default - needs time set");
            }
        }
    }
    
    // If RTC time is invalid, try to set it to compile time
    if (!rtcOk && Rtc) {
        DBGLN("RTC: Attempting to set compile time...");
        RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
        DBGF("RTC: Compile time: %04d-%02d-%02d %02d:%02d:%02d\n",
            compiled.Year(), compiled.Month(), compiled.Day(),
            compiled.Hour(), compiled.Minute(), compiled.Second());
        
        // Multiple attempts to set time
        for (int attempt = 0; attempt < 3; attempt++) {
            Rtc->SetIsWriteProtected(false);
            delay(10);
            Rtc->SetIsRunning(true);
            delay(10);
            Rtc->SetDateTime(compiled);
            delay(100);
            
            // Verify it was set correctly
            RtcDateTime verify = Rtc->GetDateTime();
            DBGF("RTC: Attempt %d - Read: %04d-%02d-%02d %02d:%02d:%02d\n",
                attempt + 1,
                verify.Year(), verify.Month(), verify.Day(),
                verify.Hour(), verify.Minute(), verify.Second());
            
            if (verify.Year() >= 2020 && verify.Year() <= 2100 && verify.Month() >= 1) {
                rtcOk = true;
                DBGLN("RTC: Time set successfully!");
                break;
            }
        }
        
        if (!rtcOk) {
            DBGLN("RTC: WARNING - Could not set time. Please use web interface to set manually.");
        }
    }
    
    rtcValid = rtcOk;  // Store RTC status globally
    
    // Validate button/sensor pins
    if (!isPinSafe(config.hardware.pinWakeupFF)) {
        DBGF("WARNING: FF pin %d may be invalid\n", config.hardware.pinWakeupFF);
    }
    if (!isPinSafe(config.hardware.pinWakeupPF)) {
        DBGF("WARNING: PF pin %d may be invalid\n", config.hardware.pinWakeupPF);
    }
    
    // Setup pins with proper pull resistors based on wakeup mode
    // Active HIGH = needs PULLDOWN (default LOW, triggered when HIGH)
    // Active LOW = needs PULLUP (default HIGH, triggered when LOW)
    if (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) {
        pinMode(config.hardware.pinWakeupFF, INPUT_PULLDOWN);
        pinMode(config.hardware.pinWakeupPF, INPUT_PULLDOWN);
        pinMode(config.hardware.pinWifiTrigger, INPUT_PULLDOWN);
    } else {
        pinMode(config.hardware.pinWakeupFF, INPUT_PULLUP);
        pinMode(config.hardware.pinWakeupPF, INPUT_PULLUP);
        pinMode(config.hardware.pinWifiTrigger, INPUT_PULLUP);
    }
    pinMode(config.hardware.pinFlowSensor, INPUT);
    
    // Validate and fix RTC if needed
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        if (!now.IsValid() || now.Year() < 2020 || now.Year() > 2100) {
            DBGLN("RTC time invalid, setting to compile time");
            RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
            Rtc->SetDateTime(compiled);
            if (!Rtc->GetIsRunning()) {
                Rtc->SetIsRunning(true);
            }
        }
        
        // Print RTC time
        now = Rtc->GetDateTime();
        DBGF("RTC Time: %04d-%02d-%02d %02d:%02d:%02d\n",
            now.Year(), now.Month(), now.Day(),
            now.Hour(), now.Minute(), now.Second());
    }
    
    DBGLN("Hardware init complete");
}

// ============================================================================
// WIFI FUNCTIONS
// ============================================================================
bool connectToWiFi() {
    if (config.network.wifiMode != WIFIMODE_CLIENT || strlen(config.network.clientSSID) == 0) {
        return false;
    }
    
    DBGF("Connecting to %s...\n", config.network.clientSSID);
    WiFi.mode(WIFI_STA);
    
    if (config.network.useStaticIP) {
        IPAddress ip(config.network.staticIP[0], config.network.staticIP[1],
                    config.network.staticIP[2], config.network.staticIP[3]);
        IPAddress gw(config.network.gateway[0], config.network.gateway[1],
                    config.network.gateway[2], config.network.gateway[3]);
        IPAddress sn(config.network.subnet[0], config.network.subnet[1],
                    config.network.subnet[2], config.network.subnet[3]);
        IPAddress dns(config.network.dns[0], config.network.dns[1],
                     config.network.dns[2], config.network.dns[3]);
        WiFi.config(ip, gw, sn, dns);
    }
    
    WiFi.begin(config.network.clientSSID, config.network.clientPassword);
    
    // Non-blocking: check connection with yield() instead of delay()
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        yield();  // Allow background tasks, non-blocking
        // Print dot every 250ms without blocking
        static unsigned long lastDot = 0;
        if (millis() - lastDot >= 250) {
            Serial.print(".");
            lastDot = millis();
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnectedAsClient = true;
        currentIPAddress = WiFi.localIP().toString();
        connectedSSID = config.network.clientSSID;
        Serial.printf("\nConnected! IP: %s\n", currentIPAddress.c_str());
        return true;
    }
    
    Serial.println("\nConnection failed");
    return false;
}

void startAPMode() {
    String apName = strlen(config.network.apSSID) > 0 ? config.network.apSSID : config.deviceName;
    DBGF("Starting AP: %s\n", apName.c_str());
    
    WiFi.mode(WIFI_AP);
    
    // Configure AP IP address
    IPAddress apIP(config.network.apIP[0], config.network.apIP[1], 
                   config.network.apIP[2], config.network.apIP[3]);
    IPAddress apGateway(config.network.apGateway[0], config.network.apGateway[1],
                        config.network.apGateway[2], config.network.apGateway[3]);
    IPAddress apSubnet(config.network.apSubnet[0], config.network.apSubnet[1],
                       config.network.apSubnet[2], config.network.apSubnet[3]);
    WiFi.softAPConfig(apIP, apGateway, apSubnet);
    
    WiFi.softAP(apName.c_str(), config.network.apPassword);
    
    currentIPAddress = WiFi.softAPIP().toString();
    wifiConnectedAsClient = false;
    DBGF("AP IP: %s\n", currentIPAddress.c_str());
}

// ============================================================================
// SAFE WIFI SHUTDOWN BEFORE RESTART
// Ð˜Ð·Ñ‡Ð¸ÑÑ‚Ð²Ð° WiFi hardware state Ð¿Ñ€ÐµÐ´Ð¸ ESP.restart().
// Ð‘ÐµÐ· Ñ‚Ð¾Ð²Ð° Ð¿Ñ€Ð¸ ÑÐ»ÐµÐ´Ð²Ð°Ñ‰ boot earlyGPIO snapshot Ð¼Ð¾Ð¶Ðµ Ð´Ð° Ñ…Ð²Ð°Ð½Ðµ WiFi Ð¿Ð¸Ð½Ð°
// ÐºÐ°Ñ‚Ð¾ HIGH Ð¸ Ð´Ð° Ð²Ð»ÐµÐ·Ðµ Ð¿Ð¾Ð³Ñ€ÐµÑˆÐ½Ð¾ Ð² Web Server Ñ€ÐµÐ¶Ð¸Ð¼.
// ============================================================================
void safeWiFiShutdown() {
    Serial.println("WiFi: Safe shutdown before restart...");
    WiFi.scanDelete();           // Ð˜Ð·Ñ‡Ð¸ÑÑ‚Ð²Ð° Ð½ÐµÐ·Ð°Ð²ÑŠÑ€ÑˆÐµÐ½ scan
    WiFi.disconnect(true);       // Disconnect + Ð¸Ð·Ñ‡Ð¸ÑÑ‚Ð²Ð° credentials Ð¾Ñ‚ RAM
    delay(50);
    WiFi.softAPdisconnect(true); // Ð¡Ð¿Ð¸Ñ€Ð° SoftAP
    delay(50);
    WiFi.mode(WIFI_OFF);         // Ð˜Ð·ÐºÐ»ÑŽÑ‡Ð²Ð° Ñ€Ð°Ð´Ð¸Ð¾Ñ‚Ð¾ Ð½Ð°Ð¿ÑŠÐ»Ð½Ð¾ â† ÐšÐ›Ð®Ð§ÐžÐ’Ðž
    delay(200);                  // Flush Ð½Ð° Ñ€Ð°Ð´Ð¸Ð¾ ÑÑ‚ÐµÐºÐ°
    Serial.println("WiFi: Radio OFF, safe to restart.");
}

// ============================================================================
// DEEP SLEEP WAKE-UP CONFIGURATION (ESP32-C3 specific)
// ============================================================================
void configureWakeup() {
    uint64_t mask = 0;
    mask |= (1ULL << config.hardware.pinWakeupFF);
    mask |= (1ULL << config.hardware.pinWakeupPF);
    mask |= (1ULL << config.hardware.pinWifiTrigger);
    
    esp_deepsleep_gpio_wake_up_mode_t mode;
    mode = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) 
           ? ESP_GPIO_WAKEUP_GPIO_HIGH 
           : ESP_GPIO_WAKEUP_GPIO_LOW;
    
    esp_deep_sleep_enable_gpio_wakeup(mask, mode);
}

String getWakeupReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        int expectedState = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
        
        // Primary: Use early GPIO bitmask captured at boot (< 1ms after wake)
        // This catches the pin state before the reed switch magnet passes
        if (earlyGPIO_captured) {
            // Extract pin states from bitmask using actual config pin numbers
            bool ffRaw   = (earlyGPIO_bitmask >> config.hardware.pinWakeupFF) & 1;
            bool pfRaw   = (earlyGPIO_bitmask >> config.hardware.pinWakeupPF) & 1;
            bool wifiRaw = (earlyGPIO_bitmask >> config.hardware.pinWifiTrigger) & 1;
            
            // Compare with expected active state
            bool ffEarly   = (expectedState == HIGH) ? ffRaw : !ffRaw;
            bool pfEarly   = (expectedState == HIGH) ? pfRaw : !pfRaw;
            bool wifiEarly = (expectedState == HIGH) ? wifiRaw : !wifiRaw;
            
            Serial.printf("GPIO Wakeup (early snapshot): FF=%d, PF=%d, WIFI=%d (expected=%s, bitmask=0x%08X)\n", 
                ffEarly, pfEarly, wifiEarly,
                expectedState == HIGH ? "HIGH" : "LOW", earlyGPIO_bitmask);
            
            // Priority: FF > PF > WIFI
            if (ffEarly) return "FF_BTN";
            if (pfEarly) return "PF_BTN";
            if (wifiEarly) return "WIFI";
            // Early snapshot shows no active pin - fall through to delayed read
            Serial.println("Early snapshot: no active pin, trying delayed read...");
        }
        
        // Fallback: Read current pin state (may be too late for momentary reed switches)
        delay(config.hardware.debounceMs);
        bool ffNow = (digitalRead(config.hardware.pinWakeupFF) == expectedState);
        bool pfNow = (digitalRead(config.hardware.pinWakeupPF) == expectedState);
        bool wifiNow = (digitalRead(config.hardware.pinWifiTrigger) == expectedState);
        
        Serial.printf("GPIO Wakeup (fallback read): FF=%d, PF=%d, WIFI=%d (expected=%s)\n", 
            ffNow, pfNow, wifiNow, 
            expectedState == HIGH ? "HIGH" : "LOW");
        
        if (ffNow) return "FF_BTN";
        if (pfNow) return "PF_BTN";
        if (wifiNow) return "WIFI";
        return "GPIO";
    }
    return (cause == ESP_SLEEP_WAKEUP_TIMER) ? "TIMER" : "PWR_ON";
}







// ============================================================================
// JSON RESPONSE HELPER
// ============================================================================
void sendJsonResponse(AsyncWebServerRequest *r, JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    r->send(200, "application/json", json);
}

// Send restart popup page with countdown
void sendRestartPage(AsyncWebServerRequest *r, const char* message) {
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Restarting</title>"
        "<style>body{font-family:-apple-system,sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;background:#f0f2f5}"
        ".popup{background:#fff;border-radius:16px;padding:2rem;text-align:center;box-shadow:0 4px 20px rgba(0,0,0,0.15);max-width:350px}"
        ".icon{font-size:4rem;margin-bottom:1rem}.title{font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem}.msg{color:#666;margin-bottom:1rem}"
        ".progress{background:#e2e8f0;border-radius:8px;height:8px;overflow:hidden;margin-top:1rem}"
        ".bar{height:100%;background:#27ae60;width:0%;transition:width 1s linear}</style></head>"
        "<body><div class='popup'><div class='icon'>ðŸ”„</div><div class='title'>Restarting...</div><div class='msg'>");
    html += message;
    html += F("</div><div id='counter'>Redirecting in 5 seconds...</div>"
        "<div class='progress'><div class='bar' id='bar'></div></div></div>"
        "<script>var s=5,b=document.getElementById('bar'),c=document.getElementById('counter');"
        "var t=setInterval(function(){s--;b.style.width=(100-s*20)+'%';c.textContent='Redirecting in '+s+' seconds...';"
        "if(s<=0){clearInterval(t);window.location.href='/';}},1000);</script></body></html>");
    r->send(200, "text/html", html);
}

// Get current page for navigation highlighting
String getCurrentPage(const char* title) {
    String t = String(title);
    if (t == "Dashboard") return "dashboard";
    if (t == "File Manager") return "files";
    if (t == "Settings" || t.startsWith("Device") || t.startsWith("Flow") || 
        t.startsWith("Hardware") || t.startsWith("Theme") || t.startsWith("Network") ||
        t.startsWith("Time") || t.startsWith("Data Log") || t.startsWith("Firmware")) return "settings";
    if (t == "Live Monitor") return "live";
    return "";
}

// Write sidebar navigation (desktop)
void writeSidebar(Print& out, const char* currentPage) {
    out.print(F("<aside class='sidebar'>"));
    out.print(F("<div class='sidebar-header'>"));
    if (strlen(config.theme.logoSource) > 0) {
        out.printf("<img src='%s' alt='' onerror=\"this.style.display='none'\">", config.theme.logoSource);
    }
    out.printf("<span class='logo'>%s</span>", config.deviceName);
    out.print(F("</div>"));
    
    out.print(F("<nav>"));
    out.print(F("<div class='nav-section'>Main</div>"));
    
    out.printf("<a href='/dashboard' class='nav-item %s'>", strcmp(currentPage, "dashboard") == 0 ? "active" : "");
    out.print(icon("ðŸ“Š"));
    out.printf("<span>%s</span></a>", "Dashboard");  // Dashboard
    
    out.printf("<a href='/files' class='nav-item %s'>", strcmp(currentPage, "files") == 0 ? "active" : "");
    out.print(icon("ðŸ“"));
    out.printf("<span>%s</span></a>", "Files");  // Files
    
    out.printf("<a href='/live' class='nav-item %s'>", strcmp(currentPage, "live") == 0 ? "active" : "");
    out.print(icon("ðŸ“¡"));
    out.printf("<span>%s</span></a>", "Live");  // Live Monitor
    
    out.print(F("<div class='nav-section'>System</div>"));
    
    out.printf("<a href='/settings' class='nav-item %s'>", strcmp(currentPage, "settings") == 0 ? "active" : "");
    out.print(icon("âš™ï¸"));
    out.printf("<span>%s</span></a>", "Settings");  // Settings
    
    out.print(F("<a href='/update' class='nav-item'>"));
    out.print(icon("ðŸ“¤"));
    out.printf("<span>%s</span></a>", "Update");  // Update
    
    out.print(F("</nav></aside>"));
}

// Write bottom navigation (mobile)
void writeBottomNav(Print& out, const char* currentPage) {
    out.print(F("<nav class='bottom-nav'>"));
    
    out.printf("<a href='/dashboard' class='%s'>", strcmp(currentPage, "dashboard") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("ðŸ“Š"));
    out.printf("</span>%s</a>", "Home");  // Home
    
    out.printf("<a href='/files' class='%s'>", strcmp(currentPage, "files") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("ðŸ“"));
    out.printf("</span>%s</a>", "Files");  // Files
    
    out.printf("<a href='/live' class='%s'>", strcmp(currentPage, "live") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("ðŸ“¡"));
    out.print(F("</span>Live</a>"));  // Keep short for mobile
    
    out.printf("<a href='/settings' class='%s'>", strcmp(currentPage, "settings") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("âš™ï¸"));
    out.printf("</span>%s</a>", "Settings");  // Settings
    
    out.print(F("</nav>"));
}

// ============================================================================
// CHUNKED HTML RESPONSE HELPER
// ============================================================================
void sendChunkedHtml(AsyncWebServerRequest *r, const char* title, std::function<void(Print&)> bodyWriter) {
    AsyncResponseStream *response = r->beginResponseStream("text/html");
    String currentPage = getCurrentPage(title);
    
    // HTML Header
    response->print(F("<!DOCTYPE html><html class='"));
    response->print(getThemeClass());
    response->print(F("'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'><title>"));
    response->print(title);
    response->print(F(" - "));
    response->print(config.deviceName);
    response->print(F("</title>"));
    // Favicon
    if (strlen(config.theme.faviconPath) > 0) {
        response->printf("<link rel='icon' href='%s'>", config.theme.faviconPath);
    }
    // CSS from external file
    response->print(F("<link rel='stylesheet' href='/style.css'>"));
    response->print(F("<style>"));
    
    // Custom colors from config
    response->printf(":root{--primary:%s;--secondary:%s}",
        config.theme.primaryColor, config.theme.secondaryColor);
    
    response->print(F("</style></head><body>"));
    
    // Sidebar (desktop)
    writeSidebar(*response, currentPage.c_str());
    
    // App Header (mobile)
    response->print(F("<header class='app-header hide-desktop'>"));
    response->print(F("<div class='logo'>"));
    if (strlen(config.theme.logoSource) > 0) {
        response->printf("<img src='%s' alt='' onerror=\"this.style.display='none'\">", config.theme.logoSource);
    }
    response->print(config.deviceName);
    response->print(F("</div>"));
    response->print(F("<div class='status'>"));
    response->print(F("<div class='time'>"));
    response->print(getRtcTimeString());
    response->print(F("</div>"));
    response->print(getNetworkDisplay());
    response->print(F("</div></header>"));
    
    // Main content area
    response->print(F("<main class='main-content'>"));
    
    // Body content via callback
    bodyWriter(*response);
    
    // Footer
    response->print(F("<footer class='app-footer'>"));
    response->print(F("<div class='footer-grid'>"));
    response->print("<span>");
    response->print(icon("ðŸ”„"));
    response->printf("Boot: %d</span>", bootCount);
    response->print("<span>");
    response->print(icon("âš¡"));
    response->printf("%dMHz</span>", getCpuFrequencyMhz());
    response->print("<span>");
    response->print(icon("ðŸ’¾"));
    response->printf("%s / %s</span>", 
        formatFileSize(ESP.getFreeHeap()).c_str(),
        formatFileSize(ESP.getHeapSize()).c_str());
    response->print(F("</div>"));
    response->print(F("<div class='footer-row'>"));
    response->print(F("<span>"));
    response->print(getNetworkDisplay());
    response->print(F("</span>"));
    response->print(F("<span style='text-align:right'>IP: "));
    response->print(currentIPAddress);
    response->print(F("</span>"));
    response->print(F("</div>"));
    response->print(F("<div class='footer-version'>Board: "));
    response->print(ESP.getChipModel());
    response->print(F(" - Firmware: "));
    response->print(getVersionString());
    response->print(F("</div></footer>"));
    
    response->print(F("</main>"));
    
    // Bottom Navigation (mobile)
    writeBottomNav(*response, currentPage.c_str());
    
    response->print(F("</body></html>"));
    r->send(response);
}

// ============================================================================
// FILE LIST GENERATOR
// ============================================================================

// Get filesystem for current view
fs::FS* getCurrentViewFS() {
    if (currentStorageView == "sdcard" && sdAvailable) {
        return &SD;
    } else if (littleFsAvailable) {
        return &LittleFS;
    }
    return nullptr;
}

void writeFileList(Print& out, const String& dir, bool editMode = false) {
    fs::FS* viewFS = getCurrentViewFS();
    if (!viewFS) {
        out.print(F("<p>Storage not available</p>"));
        return;
    }
    
    File d = viewFS->open(dir);
    if (!d || !d.isDirectory()) {
        out.print(F("<p>Cannot open directory</p>"));
        return;
    }
    
    bool hasFiles = false;
    while (File entry = d.openNextFile()) {
        hasFiles = true;
        String name = String(entry.name());
        String fullPath = buildPath(dir, name);
        bool isDir = entry.isDirectory();
        size_t size = entry.size();
        entry.close();
        
        out.print(F("<div class='list-item'><span>"));
        out.print(isDir ? "ðŸ“ " : "ðŸ“„ ");
        out.print(name);
        if (!isDir) {
            out.print(F(" <small class='text-muted'>("));
            out.print(formatFileSize(size));
            out.print(F(")</small>"));
        }
        out.print(F("</span><span class='btn-group'>"));
        
        // Basic actions (always visible)
        if (isDir) {
            out.printf("<a href='/files?storage=%s&dir=%s%s' class='btn btn-sm btn-primary'>Open</a>", 
                currentStorageView.c_str(), fullPath.c_str(), editMode ? "&edit=1" : "");
        } else {
            out.printf("<a href='/download?storage=%s&file=%s' class='btn btn-sm btn-secondary'>ðŸ“¥</a>", 
                currentStorageView.c_str(), fullPath.c_str());
        }
        
        // Edit actions (only if edit mode)
        if (editMode) {
            // Move/Rename button - opens popup
            out.printf("<button class='btn btn-sm btn-secondary' onclick=\"showMove('%s','%s')\">âœ‚ï¸</button>", 
                fullPath.c_str(), name.c_str());
            
            // Delete button
            out.printf("<a href='/delete?storage=%s&path=%s&dir=%s' class='btn btn-sm btn-danger' onclick='return confirm(\"Delete %s?\")'>ðŸ—‘ï¸</a>", 
                currentStorageView.c_str(), fullPath.c_str(), dir.c_str(), name.c_str());
        }
        out.print(F("</span></div>"));
    }
    d.close();
    
    if (!hasFiles) out.print(F("<div class='list-item text-muted'>Empty directory</div>"));
}

// ============================================================================
// STORAGE INFO
// ============================================================================
void getStorageInfo(uint64_t &used, uint64_t &total, int &percent, const String& storageType = "") {
    used = 0; total = 0; percent = 0;
    
    String sType = storageType;
    if (sType.isEmpty()) {
        // Default to active storage
        sType = (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) ? "sdcard" : "internal";
    }
    
    if (sType == "sdcard" && sdAvailable) {
        used = SD.usedBytes();
        // Use cardSize() for actual SD card capacity (totalBytes has ~4GB limit)
        total = SD.cardSize();
    } else if (sType == "internal" && littleFsAvailable) {
        used = LittleFS.usedBytes();
        total = LittleFS.totalBytes();
    }
    
    if (total > 0) percent = (used * 100ULL) / total;
}

String getStorageBarColor(int percent) {
    if (percent >= 90) return config.theme.storageBar90Color;
    if (percent >= 70) return config.theme.storageBar70Color;
    return config.theme.storageBarColor;
}

// ============================================================================
// DATALOG FILE HELPERS
// ============================================================================
String generateDatalogFileOptions() {
    String html = "";
    if (!fsAvailable || !activeFS) return "<option>No storage</option>";
    
    String currentFile = getActiveDatalogFile();
    
    // Recursive function to scan directories
    std::function<void(const String&)> scanDir = [&](const String& path) {
        File dir = activeFS->open(path);
        if (!dir || !dir.isDirectory()) return;
        
        while (File entry = dir.openNextFile()) {
            String name = String(entry.name());
            String fullPath = path == "/" ? "/" + name : path + "/" + name;
            
            if (entry.isDirectory()) {
                scanDir(fullPath);
            } else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) {
                String selected = (fullPath == currentFile) ? "selected" : "";
                html += "<option value='" + fullPath + "' " + selected + ">" + fullPath + "</option>";
            }
            entry.close();
        }
        dir.close();
    };
    
    scanDir("/");
    
    return html.length() > 0 ? html : "<option value='/datalog.txt'>datalog.txt</option>";
}

int countDatalogFiles() {
    if (!fsAvailable || !activeFS) return 0;
    
    int count = 0;
    std::function<void(const String&)> scanDir = [&](const String& path) {
        File dir = activeFS->open(path);
        if (!dir || !dir.isDirectory()) return;
        
        while (File entry = dir.openNextFile()) {
            String name = String(entry.name());
            String fullPath = path == "/" ? "/" + name : path + "/" + name;
            
            if (entry.isDirectory()) {
                scanDir(fullPath);
            } else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) {
                count++;
            }
            entry.close();
        }
        dir.close();
    };
    
    scanDir("/");
    return count;
}

// ============================================================================
// RTC TIME STRING
// ============================================================================
String getRtcTimeString() {
    if (!Rtc) {
        return "No RTC";
    }
    RtcDateTime now = Rtc->GetDateTime();
    // Check for valid time - Year 2000 with Month/Day 0 means unset
    if (now.Year() < 2020 || now.Year() > 2100 || now.Month() == 0 || now.Day() == 0) {
        return "Set Time";
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", now.Hour(), now.Minute(), now.Second());
    return String(buf);
}

String getRtcDateTimeString() {
    if (!Rtc) return "No RTC";
    RtcDateTime now = Rtc->GetDateTime();
    // Year 2000 with Month/Day 0 means RTC is not set
    if (now.Year() < 2020 || now.Year() > 2100 || now.Month() == 0 || now.Day() == 0) {
        return "Not Set - Use Manual Set";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", 
        now.Year(), now.Month(), now.Day(),
        now.Hour(), now.Minute(), now.Second());
    return String(buf);
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================
void setupWebServer() {
    Serial.println("Setting up web server...");
    
    // Root redirect to Statistics (main dashboard)
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/dashboard"); });
    
    // ========== DASHBOARD (Statistics - Main Page) ==========
    server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Dashboard", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“Š"));
            out.printf("%s</h1></div>", "Dashboard");
            
            // File selector card
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;gap:1rem;flex-wrap:wrap;align-items:center'>"));
            out.print(F("<select id='fileSelect' class='form-input form-select' style='flex:1;min-width:200px' onchange='loadData()'>"));
            out.print(generateDatalogFileOptions());
            out.print(F("</select>"));
            out.print(F("<button class='btn btn-primary' onclick='loadData()'>ðŸ”„</button>"));
            out.print(F("</div></div></div>"));
            
            // Filters card
            out.printf("<div class='card'><div class='card-header'>ðŸ” %s</div><div class='card-body'>", "Filters");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Start");
            out.print(F("<input type='date' id='startDate' class='form-input' onchange='applyFilters()'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "End");
            out.print(F("<input type='date' id='endDate' class='form-input' onchange='applyFilters()'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Trigger");
            out.print(F("<select id='eventFilter' class='form-input form-select' onchange='applyFilters()'>"));
            out.printf("<option value='ALL'>%s</option>", "All");
            out.printf("<option value='BTN'>%s</option>", "Buttons Only");
            out.printf("<option value='FF'>%s</option>", "Full Flush Btn");
            out.printf("<option value='PF'>%s</option>", "Part Flush Btn");
            out.print(F("</select></div>"));
            out.print(F("<div class='form-group'><label class='form-label'>Extra presses</label>"));
            out.print(F("<select id='pressFilter' class='form-input form-select' onchange='applyFilters()'>"));
            out.printf("<option value='ALL'>%s</option>", "All");
            out.printf("<option value='EXTRA'>%s</option>", "With Extra presses");
            out.printf("<option value='NONE'>%s</option>", "No Extra presses");
            out.print(F("</select></div>"));
            out.print(F("</div>"));
            // Exclude zero checkbox
            out.print(F("<div style='margin-top:8px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>"));
            out.print(F("<label style='display:flex;align-items:center;gap:6px;cursor:pointer'>"));
            out.print(F("<input type='checkbox' id='excludeZero' onchange='applyFilters()' checked>"));
            out.print(F("<span>Exclude 0.00L entries</span></label>"));
            out.print(F("<button onclick='exportCSV()' class='btn btn-sm btn-secondary'>ðŸ“¥ Export CSV</button>"));
            out.print(F("</div>"));
            out.print(F("</div></div>"));
            
            out.print(F("<div id='errorMsg' class='alert alert-error' style='display:none'></div>"));
            
            // Stats cards
            out.print(F("<div class='stats-grid mb-2'>"));
            out.printf("<div class='stat-card'><div class='value' id='totalVol'>0.00</div><div class='label'>%s</div></div>", "Liters");
            out.printf("<div class='stat-card'><div class='value' id='eventCount'>0</div><div class='label'>%s</div></div>", "Events");
            out.printf("<div class='stat-card'><div class='value' id='totalFF' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.ffColor, "Extra FF");
            out.printf("<div class='stat-card'><div class='value' id='totalPF' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.pfColor, "Extra PF");
            out.print(F("</div>"));
            
            // Chart card
            out.printf("<div class='card'><div class='card-header'>ðŸ“ˆ %s</div>", "Volume");
            out.print(F("<div class='card-body' style='height:350px'><canvas id='chart'></canvas></div>"));
            out.print(F("<div class='card-body' style='padding-top:0'>"));
            out.print(F("<div style='display:flex;gap:1rem;justify-content:center;flex-wrap:wrap;font-size:.85rem'>"));
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.ffColor, "Full Flush Btn");
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.pfColor, "Part Flush Btn");
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.otherColor, "Other");
            out.print(F("</div></div></div>"));
            
            // Chart.js loading
            if (config.theme.chartSource == CHART_LOCAL) {
                out.printf("<script src='%s'></script>", config.theme.chartLocalPath);
            } else {
                out.print(F("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"));
            }
            
            // JavaScript
            out.print(F("<script>var chart=null,rawData='',filteredData=[];"));
            out.printf("var deviceId='%s';", config.deviceId);
            out.printf("var colors={ff:'%s',pf:'%s',other:'%s'};", config.theme.ffColor, config.theme.pfColor, config.theme.otherColor);
            // JS translation strings
            out.printf("var lang={trigger:'%s',liters:'%s',extraFF:'%s',extraPF:'%s'};", 
                "Trigger", "Liters", "Extra FF", "Extra PF");
            
            out.print(F("function loadData(){var file=document.getElementById('fileSelect').value;"));
            out.print(F("document.getElementById('errorMsg').style.display='none';"));
            out.print(F("fetch('/download?file='+encodeURIComponent(file)).then(r=>{if(!r.ok)throw new Error('Failed');return r.text();})"));
            out.print(F(".then(data=>{rawData=data;applyFilters();})"));
            out.print(F(".catch(e=>{document.getElementById('errorMsg').textContent='Error loading: '+e.message;document.getElementById('errorMsg').style.display='block';});}"));
            
            out.print(F("function applyFilters(){if(!rawData){loadData();return;}processData(rawData);}"));
            
            // Flexible processData - auto-detects format from each line
            out.print(F("function processData(data){"));
            out.print(F("var lines=data.trim().split('\\n'),filtered=[],startVal=document.getElementById('startDate').value,"));
            out.print(F("endVal=document.getElementById('endDate').value,filterType=document.getElementById('eventFilter').value,"));
            out.print(F("pressType=document.getElementById('pressFilter').value,"));
            out.print(F("excludeZero=document.getElementById('excludeZero').checked,"));
            out.print(F("tVol=0,tFF=0,tPF=0;"));
            out.print(F("lines.forEach(function(line){"));
            out.print(F("var p=line.split('|');if(p.length<2)return;"));
            // Auto-detect date format
            out.print(F("var dateStr='',timeStr='',endStr='',boot='',reason='',vol=0,ff=0,pf=0,i=0;"));
            // Check first field for date (contains / or - or . with 4-digit year)
            out.print(F("if(p[0].match(/\\d{2}[\\/\\.\\-]\\d{2}[\\/\\.\\-]\\d{4}/)||p[0].match(/\\d{4}\\-\\d{2}\\-\\d{2}/)){dateStr=p[0];i=1;}"));
            // Next is time (contains : )
            out.print(F("if(p[i]&&p[i].indexOf(':')>=0){timeStr=p[i];i++;}"));
            // Check for end time/duration (has : or ends with s)
            out.print(F("if(p[i]&&(p[i].indexOf(':')>=0||p[i].match(/^\\d+s$/))){endStr=p[i];i++;}"));
            // Check for boot count (#:)
            out.print(F("if(p[i]&&p[i].indexOf('#:')===0){boot=p[i].substring(2);i++;}"));
            // Next should be trigger (FF_BTN, PF_BTN, IDLE, etc)
            out.print(F("if(p[i]&&(p[i].indexOf('FF')>=0||p[i].indexOf('PF')>=0||p[i]==='IDLE')){reason=p[i];i++;}"));
            // Volume (L: prefix or just number)
            out.print(F("if(p[i]){var vStr=p[i].replace('L:','').replace(',','.');vol=parseFloat(vStr)||0;i++;}"));
            // Extra FF
            out.print(F("if(p[i]&&p[i].indexOf('FF')===0){ff=parseInt(p[i].replace('FF',''))||0;i++;}"));
            // Extra PF
            out.print(F("if(p[i]&&p[i].indexOf('PF')===0){pf=parseInt(p[i].replace('PF',''))||0;}"));
            // Parse date for filtering
            out.print(F("var entryDate='';"));
            out.print(F("if(dateStr){var m;"));
            out.print(F("if(m=dateStr.match(/(\\d{2})[\\/\\.](\\d{2})[\\/\\.](\\d{4})/))entryDate=m[3]+'-'+m[2]+'-'+m[1];"));
            out.print(F("else if(m=dateStr.match(/(\\d{4})\\-(\\d{2})\\-(\\d{2})/))entryDate=m[1]+'-'+m[2]+'-'+m[3];}"));
            // Apply filters
            out.print(F("if(startVal&&entryDate&&entryDate<startVal)return;if(endVal&&entryDate&&entryDate>endVal)return;"));
            out.print(F("if(filterType==='BTN'&&reason.indexOf('FF')===-1&&reason.indexOf('PF')===-1)return;"));
            out.print(F("if(filterType==='FF'&&reason.indexOf('FF')===-1)return;"));
            out.print(F("if(filterType==='PF'&&reason.indexOf('PF')===-1)return;"));
            out.print(F("if(pressType==='EXTRA'&&ff===0&&pf===0)return;"));
            out.print(F("if(pressType==='NONE'&&(ff>0||pf>0))return;"));
            out.print(F("if(excludeZero&&vol===0)return;"));
            out.print(F("tFF+=ff;tPF+=pf;tVol+=vol;"));
            // time = start only, fullTime = start-end for CSV
            out.print(F("var fullTime=timeStr+(endStr?'-'+endStr:'');"));
            out.print(F("filtered.push({date:dateStr||'N/A',time:timeStr,fullTime:fullTime,boot:boot,vol:vol,reason:reason,ff:ff,pf:pf});});"));
            out.print(F("filteredData=filtered;"));
            out.print(F("document.getElementById('totalVol').textContent=tVol.toFixed(2)+' L';"));
            out.print(F("document.getElementById('eventCount').textContent=filtered.length;"));
            out.print(F("document.getElementById('totalFF').textContent=tFF;"));
            out.print(F("document.getElementById('totalPF').textContent=tPF;"));
            out.print(F("renderChart(filtered);}"));
            
            // Export CSV function - uses fullTime and boot
            out.print(F("function exportCSV(){if(!filteredData.length){alert('No data to export');return;}"));
            out.print(F("var csv='Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\\n';"));
            out.print(F("filteredData.forEach(function(d){csv+=d.date+','+d.fullTime+','+(d.boot||'')+','+d.vol.toFixed(2)+','+d.reason+','+d.ff+','+d.pf+'\\n';});"));
            // Build filename: deviceId_filters_date.csv
            out.print(F("var f=deviceId;"));
            out.print(F("var ft=document.getElementById('eventFilter').value;if(ft!=='ALL')f+='_'+ft;"));
            out.print(F("var pt=document.getElementById('pressFilter').value;if(pt!=='ALL')f+='_'+pt;"));
            out.print(F("if(document.getElementById('excludeZero').checked)f+='_noZero';"));
            out.print(F("var sd=document.getElementById('startDate').value;if(sd)f+='_from'+sd;"));
            out.print(F("var ed=document.getElementById('endDate').value;if(ed)f+='_to'+ed;"));
            out.print(F("var today=new Date().toISOString().slice(0,10);f+='_'+today+'.csv';"));
            out.print(F("var blob=new Blob([csv],{type:'text/csv'});var url=URL.createObjectURL(blob);"));
            out.print(F("var a=document.createElement('a');a.href=url;a.download=f;a.click();URL.revokeObjectURL(url);}")); 
            
            // renderChart with configurable label format
            out.print(F("function renderChart(data){"));
            out.print(F("var ctx=document.getElementById('chart').getContext('2d');"));
            out.print(F("if(chart)chart.destroy();"));
            out.print(F("var clr=data.map(d=>{if(d.reason.indexOf('FF')>=0)return colors.ff;if(d.reason.indexOf('PF')>=0)return colors.pf;return colors.other;});"));
            // Label format: 0=DateTime, 1=BootCount, 2=Both
            out.printf("var lblFmt=%d;", config.theme.chartLabelFormat);
            out.print(F("var lbls=data.map(d=>{"));
            out.print(F("if(lblFmt===1)return d.boot?'#'+d.boot:'#?';"));
            out.print(F("if(lblFmt===2)return(d.date+' '+d.time+(d.boot?' #'+d.boot:''));"));
            out.print(F("return d.date+' '+d.time;});"));
            out.print(F("chart=new Chart(ctx,{type:'bar',data:{labels:lbls,datasets:[{"));
            out.print(F("label:lang.liters+' (L)',data:data.map(d=>d.vol),backgroundColor:clr,borderWidth:0}]},"));
            out.print(F("options:{responsive:true,maintainAspectRatio:false,plugins:{tooltip:{callbacks:{"));
            out.print(F("afterLabel:ctx=>{var d=data[ctx.dataIndex];return[lang.trigger+': '+d.reason,'Boot: '+(d.boot||'N/A'),lang.extraFF+': '+d.ff,lang.extraPF+': '+d.pf];}"));
            out.print(F("}}},scales:{y:{beginAtZero:true,title:{display:true,text:lang.liters}}}}});}"));
            
            out.print(F("window.onload=loadData;"));
            out.print(F("</script>"));
        });
    });
    
    // ========== FILE MANAGER ==========
    server.on("/files", HTTP_GET, [](AsyncWebServerRequest *r) {
        currentDir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        currentDir = sanitizeFilename(currentDir);
        if (!currentDir.startsWith("/")) currentDir = "/" + currentDir;
        bool editMode = r->hasParam("edit") && r->getParam("edit")->value() == "1";
        
        // Storage selection
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        } else {
            // If no parameter, use the setting from Device Settings
            if (config.hardware.defaultStorageView == 1 && sdAvailable) {
                currentStorageView = "sdcard";
            } else {
                currentStorageView = "internal";
            }
        }
        // Validate storage view
        if (currentStorageView == "sdcard" && !sdAvailable) {
            currentStorageView = "internal";
        }
        if (currentStorageView == "internal" && !littleFsAvailable) {
            currentStorageView = sdAvailable ? "sdcard" : "internal";
        }
        
        sendChunkedHtml(r, "Files", [editMode](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“"));
            out.printf("%s</h1></div>", "File Manager");
            
            // Status message
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            // Storage tabs (only show if both available)
            if (littleFsAvailable || sdAvailable) {
                out.print(F("<div class='tabs' style='display:flex;gap:4px;margin-bottom:1rem'>"));
                
                if (littleFsAvailable) {
                    out.print(F("<a href='/files?storage=internal&dir=/' class='btn "));
                    out.print(currentStorageView == "internal" ? "btn-primary" : "btn-secondary");
                    out.printf("'>ðŸ’¾ %s</a>", "Internal");
                }
                
                if (sdAvailable) {
                    out.print(F("<a href='/files?storage=sdcard&dir=/' class='btn "));
                    out.print(currentStorageView == "sdcard" ? "btn-primary" : "btn-secondary");
                    out.printf("'>ðŸ’³ %s</a>", "SD Card");
                }
                
                out.print(F("</div>"));
            }
            
            // Storage info card
            uint64_t used, total; int percent;
            getStorageInfo(used, total, percent, currentStorageView);
            
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px'>"));
            out.print(F("<span>"));
            out.print(formatFileSize(used));
            out.print(F(" / "));
            out.print(formatFileSize(total));
            out.print(F("</span>"));
            out.printf("<span class='text-muted'>%d%%</span>", percent);
            out.print(F("</div>"));
            out.print(F("<div class='progress'><div class='progress-bar"));
            if (percent >= 90) out.print(F(" progress-bar-danger"));
            else if (percent >= 70) out.print(F(" progress-bar-warning"));
            else out.print(F(" progress-bar-success"));
            out.printf("' style='width:%d%%'></div></div>", percent);
            out.print(F("</div></div>"));
            
            // Directory card
            out.print(F("<div class='card'>"));
            out.print(F("<div class='card-header' style='display:flex;justify-content:space-between;align-items:center'>"));
            out.print(F("<span>ðŸ“‚ "));
            out.print(currentStorageView == "sdcard" ? "[SD] " : "[Int] ");
            out.print(currentDir == "/" ? "Root" : currentDir.c_str());
            out.print(F("</span>"));
            out.print(F("<div class='btn-group'>"));
            if (currentDir != "/") {
                int lastSlash = currentDir.lastIndexOf('/');
                String parent = lastSlash <= 0 ? "/" : currentDir.substring(0, lastSlash);
                if (editMode) {
                    out.printf("<a href='/files?storage=%s&dir=%s&edit=1' class='btn btn-sm btn-secondary'>â¬†ï¸ Up</a>", currentStorageView.c_str(), parent.c_str());
                } else {
                    out.printf("<a href='/files?storage=%s&dir=%s' class='btn btn-sm btn-secondary'>â¬†ï¸ Up</a>", currentStorageView.c_str(), parent.c_str());
                }
            }
            if (editMode) {
                out.printf("<a href='/files?storage=%s&dir=%s' class='btn btn-sm btn-secondary'>âœ–ï¸ Done</a>", currentStorageView.c_str(), currentDir.c_str());
            } else {
                out.printf("<a href='/files?storage=%s&dir=%s&edit=1' class='btn btn-sm btn-primary'>âœï¸ Edit</a>", currentStorageView.c_str(), currentDir.c_str());
            }
            out.print(F("</div></div>"));
            
            out.print(F("<div class='card-body' style='padding:0'>"));
            writeFileList(out, currentDir, editMode);
            out.print(F("</div>"));
            
            // Upload/Create forms (edit mode only)
            if (editMode) {
                out.print(F("<div class='card-body' style='border-top:1px solid var(--border);background:var(--bg)'>"));
                out.print(F("<div style='display:flex;gap:1rem;flex-wrap:wrap;align-items:center'>"));
                out.print(F("<form id='fileUploadForm' action='/upload' method='POST' enctype='multipart/form-data' style='display:flex;gap:8px;align-items:center'>"));
                out.printf("<input type='hidden' name='path' value='%s'>", currentDir.c_str());
                out.print(F("<input type='file' name='file' id='fileInput' required class='form-input' style='width:auto'>"));
                out.print(F("<button type='submit' class='btn btn-sm btn-success'>ðŸ“¤ Upload</button></form>"));
                out.print(F("<form action='/mkdir' method='GET' style='display:flex;gap:8px;align-items:center'>"));
                out.printf("<input type='hidden' name='dir' value='%s'>", currentDir.c_str());
                out.print(F("<input type='text' name='name' placeholder='New folder' required class='form-input' style='width:120px'>"));
                out.print(F("<button type='submit' class='btn btn-sm btn-primary'>ðŸ“ Create</button></form>"));
                out.print(F("</div>"));
                // Upload progress bar
                out.print(F("<div id='uploadProg' style='display:none;margin-top:8px'>"));
                out.print(F("<div style='display:flex;align-items:center;gap:8px'>"));
                out.print(F("<div class='progress' style='flex:1;height:10px'><div id='uploadBar' class='progress-bar progress-bar-success' style='width:0%'></div></div>"));
                out.print(F("<span id='uploadPct' style='font-size:0.8rem;min-width:45px'>0%</span></div></div>"));
                out.print(F("</div>"));
                
                // ===== MOVE/RENAME POPUP =====
                out.print(F("<div id='movePopup' class='popup-overlay'>"));
                out.print(F("<div class='popup-content' style='text-align:left;max-width:400px'>"));
                out.print(F("<div style='font-size:2.5rem;text-align:center;margin-bottom:0.5rem'>âœ‚ï¸</div>"));
                out.print(F("<div style='font-size:1.2rem;font-weight:bold;text-align:center;margin-bottom:1rem'>Move / Rename</div>"));
                out.print(F("<form action='/move_file' method='GET'>"));
                out.printf("<input type='hidden' name='storage' value='%s'>", currentStorageView.c_str());
                out.printf("<input type='hidden' name='dir' value='%s'>", currentDir.c_str());
                out.print(F("<input type='hidden' name='src' id='mvSrc'>"));
                out.print(F("<div class='form-group'><label class='form-label'>New Name</label>"));
                out.print(F("<input type='text' name='newName' id='mvName' class='form-input' required></div>"));
                out.print(F("<div class='form-group'><label class='form-label'>Move to Folder</label>"));
                out.print(F("<select name='destDir' id='mvDest' class='form-input form-select'>"));
                out.print(F("<option value=''>(same location)</option>"));
                out.print(F("<option value='/'>/ (root)</option>"));
                // List folders from current storage
                fs::FS* viewFS = getCurrentViewFS();
                if (viewFS) {
                    std::function<void(const String&, int)> listDirs = [&](const String& path, int depth) {
                        if (depth > 3) return;
                        File d = viewFS->open(path);
                        if (d && d.isDirectory()) {
                            while (File f = d.openNextFile()) {
                                if (f.isDirectory()) {
                                    String fp = buildPath(path, String(f.name()));
                                    out.print(F("<option value='")); out.print(fp); out.print(F("'>"));
                                    for(int i=0;i<depth;i++) out.print(F("&nbsp;&nbsp;"));
                                    out.print(f.name()); out.print(F("</option>"));
                                    listDirs(fp, depth + 1);
                                }
                                f.close();
                            }
                            d.close();
                        }
                    };
                    listDirs("/", 1);
                }
                out.print(F("</select></div>"));
                out.print(F("<div style='display:flex;gap:0.5rem;justify-content:flex-end;margin-top:1rem'>"));
                out.print(F("<button type='button' class='btn btn-secondary' onclick=\"hidePopup('movePopup')\">Cancel</button>"));
                out.print(F("<button type='submit' class='btn btn-primary'>âœ‚ï¸ Apply</button>"));
                out.print(F("</div></form></div></div>"));
                
                // Upload progress script + popup functions
                out.print(F("<script>"));
                out.print(F("document.getElementById('fileUploadForm').onsubmit=function(e){"));
                out.print(F("e.preventDefault();var f=document.getElementById('fileInput').files[0];if(!f)return;"));
                out.print(F("var xhr=new XMLHttpRequest(),fd=new FormData(this);"));
                out.print(F("document.getElementById('uploadProg').style.display='block';"));
                out.print(F("xhr.upload.onprogress=function(ev){if(ev.lengthComputable){"));
                out.print(F("var p=Math.round(ev.loaded/ev.total*100);"));
                out.print(F("document.getElementById('uploadBar').style.width=p+'%';"));
                out.print(F("document.getElementById('uploadPct').textContent=p+'%';}};"));
                out.print(F("xhr.onload=function(){setTimeout(function(){location.reload();},500);};"));
                out.print(F("xhr.onerror=function(){alert('Upload failed');location.reload();};"));
                out.print(F("xhr.open('POST','/upload');xhr.send(fd);};"));
                // Popup helper functions
                out.print(F("function showPopup(id){document.getElementById(id).style.display='flex';}"));
                out.print(F("function hidePopup(id){document.getElementById(id).style.display='none';}"));
                out.print(F("function showMove(path,name){document.getElementById('mvSrc').value=path;"));
                out.print(F("document.getElementById('mvName').value=name;showPopup('movePopup');}"));
                out.print(F("</script>"));
            }
            out.print(F("</div>"));
        });
    });
    
    // ========== SETTINGS MENU ==========
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Settings", [](Print& out) {
            // Restart confirmation popup
            out.print(F("<div id='restartPopup' class='popup-overlay'>"));
            out.print(F("<div class='popup-content'>"));
            out.print(F("<div id='popupIcon' style='font-size:4rem;margin-bottom:1rem'>ðŸ”„</div>"));
            out.print(F("<div id='popupTitle' style='font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem'>Restart Device?</div>"));
            out.print(F("<div id='popupMsg' style='margin-bottom:1rem'>The device will restart. Any unsaved changes will be lost.</div>"));
            out.print(F("<div id='popupProgress' style='display:none'>"));
            out.print(F("<div style='background:#e0e0e0;border-radius:4px;height:12px;margin-bottom:0.5rem;overflow:hidden'><div id='popupBar' style='width:0%;height:100%;background:#f39c12;transition:width 0.3s'></div></div>"));
            out.print(F("</div>"));
            out.print(F("<div id='popupButtons' style='display:flex;gap:1rem;justify-content:center'>"));
            out.print(F("<button class='btn btn-danger' onclick='confirmRestart()'>âœ“ Confirm</button>"));
            out.print(F("<button class='btn btn-secondary' onclick='cancelRestart()'>âœ— Cancel</button>"));
            out.print(F("</div></div></div>"));
            
            // Page header with restart button
            out.print(F("<div class='page-header' style='display:flex;justify-content:space-between;align-items:flex-start'>"));
            out.print(F("<div><h1>"));
            out.print(icon("âš™ï¸"));
            out.print("Settings");
            out.print(F("</h1></div>"));
            out.printf("<button onclick='showRestartPopup()' class='btn btn-warning'>ðŸ”„ %s</button>", "Restart");
            out.print(F("</div>"));
            
            // Settings grid
            out.print(F("<div class='settings-grid'>"));
            
            // Device
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_device'\"><div class='icon'>"));
            out.print(icon("ðŸ“±"));
            out.printf("</div><div class='title'>%s</div></div>", "Device");
            
            // Flow Meter
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_flowmeter'\"><div class='icon'>"));
            out.print(icon("ðŸ’§"));
            out.printf("</div><div class='title'>%s</div></div>", "Flow Meter");
            
            // Hardware
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_hardware'\"><div class='icon'>"));
            out.print(icon("ðŸ”§"));
            out.printf("</div><div class='title'>%s</div></div>", "Hardware");
            
            // Data Log
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_datalog'\"><div class='icon'>"));
            out.print(icon("ðŸ“"));
            out.printf("</div><div class='title'>%s</div></div>", "Data Log");
            
            // Theme
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_theme'\"><div class='icon'>"));
            out.print(icon("ðŸŽ¨"));
            out.printf("</div><div class='title'>%s</div></div>", "Theme");
            
            // Network
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_network'\"><div class='icon'>"));
            out.print(icon("ðŸŒ"));
            out.printf("</div><div class='title'>%s</div></div>", "Network");
            
            // Time Sync
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_time'\"><div class='icon'>"));
            out.print(icon("ðŸ•"));
            out.printf("</div><div class='title'>%s</div></div>", "Time Sync");
            
            // Update
            out.print(F("<div class='setting-card' onclick=\"location.href='/update'\"><div class='icon'>"));
            out.print(icon("ðŸ“¤"));
            out.printf("</div><div class='title'>%s</div></div>", "Update");
            
            out.print(F("</div>"));
            
            // Backup & Restore card
            out.print(F("<div class='card mt-2'><div class='card-header'>ðŸ’¾ Backup & Restore</div>"));
            out.print(F("<div class='card-body'>"));
            out.print(F("<div class='btn-group' style='margin-bottom:1rem'>"));
            out.printf("<a href='/export_settings' download='%s_settings.json' class='btn btn-primary'>ðŸ“¥ Export Settings</a>", config.deviceName);
            out.print(F("</div>"));
            out.print(F("<form id='importForm' action='/import_settings' method='POST' enctype='multipart/form-data'>"));
            out.print(F("<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>"));
            out.print(F("<input type='file' name='settings' id='importFile' accept='.json' class='form-input' style='flex:1;min-width:150px'>"));
            out.print(F("<button type='submit' class='btn btn-secondary'>ðŸ“¤ Import Settings</button></div>"));
            out.print(F("</form>"));
            // Import progress bar
            out.print(F("<div id='importProg' style='display:none;margin-top:8px'>"));
            out.print(F("<div style='display:flex;align-items:center;gap:8px'>"));
            out.print(F("<div class='progress' style='flex:1;height:10px'><div id='importBar' class='progress-bar progress-bar-success' style='width:0%'></div></div>"));
            out.print(F("<span id='importPct' style='font-size:0.8rem;min-width:45px'>0%</span></div></div>"));
            out.print(F("</div></div>"));
            
            // Import progress script
            out.print(F("<script>"));
            out.print(F("document.getElementById('importForm').onsubmit=function(e){"));
            out.print(F("e.preventDefault();var f=document.getElementById('importFile').files[0];if(!f)return;"));
            out.print(F("var xhr=new XMLHttpRequest(),fd=new FormData(this);"));
            out.print(F("document.getElementById('importProg').style.display='block';"));
            out.print(F("xhr.upload.onprogress=function(ev){if(ev.lengthComputable){"));
            out.print(F("var p=Math.round(ev.loaded/ev.total*100);"));
            out.print(F("document.getElementById('importBar').style.width=p+'%';"));
            out.print(F("document.getElementById('importPct').textContent=p+'%';}};"));
            out.print(F("xhr.onload=function(){"));
            out.print(F("if(xhr.status===200){alert('Settings imported successfully!');location.reload();}"));
            out.print(F("else{alert('Import failed: '+xhr.responseText);document.getElementById('importProg').style.display='none';}};"));
            out.print(F("xhr.onerror=function(){alert('Import failed');document.getElementById('importProg').style.display='none';};"));
            out.print(F("xhr.open('POST','/import_settings');xhr.send(fd);};"));
            out.print(F("</script>"));
            
            // JavaScript for restart popup
            out.print(F("<script>"));
            out.print(F("function showRestartPopup(){"));
            out.print(F("document.getElementById('popupIcon').textContent='ðŸ”„';"));
            out.print(F("document.getElementById('popupTitle').textContent='Restart Device?';"));
            out.print(F("document.getElementById('popupMsg').textContent='The device will restart. Any unsaved changes will be lost.';"));
            out.print(F("document.getElementById('popupProgress').style.display='none';"));
            out.print(F("document.getElementById('popupButtons').style.display='flex';"));
            out.print(F("document.getElementById('restartPopup').style.display='flex';"));
            out.print(F("}"));
            out.print(F("function cancelRestart(){"));
            out.print(F("document.getElementById('restartPopup').style.display='none';"));
            out.print(F("}"));
            out.print(F("function confirmRestart(){"));
            out.print(F("document.getElementById('popupButtons').style.display='none';"));
            out.print(F("document.getElementById('popupProgress').style.display='block';"));
            out.print(F("document.getElementById('popupIcon').textContent='â³';"));
            out.print(F("document.getElementById('popupTitle').textContent='Restarting...';"));
            out.print(F("var seconds=5;"));
            out.print(F("var tick=function(){"));
            out.print(F("document.getElementById('popupMsg').innerHTML='Redirecting in <strong>'+seconds+'</strong> seconds...';"));
            out.print(F("document.getElementById('popupBar').style.width=((5-seconds)*20)+'%';"));
            out.print(F("if(seconds<=0){fetch('/restart').then(()=>{window.location.href='/dashboard';}).catch(()=>{window.location.href='/dashboard';});}"));
            out.print(F("else{seconds--;setTimeout(tick,1000);}"));
            out.print(F("};tick();"));
            out.print(F("}"));
            out.print(F("</script>"));
        });
    });
    
    // Restart device (supports both AJAX and direct access)
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Check if it's an AJAX request or direct browser access
        bool isAjax = r->hasHeader("X-Requested-With") || 
                      (r->hasHeader("Accept") && r->header("Accept").indexOf("application/json") >= 0);
        
        if (isAjax) {
            // AJAX request - return JSON and restart
            r->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting...\"}");
        } else {
            // Direct browser access - show page with redirect
            r->send(200, "text/html", 
                "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Restarting</title><style>body{font-family:system-ui,sans-serif;text-align:center;padding:50px;background:#f0f2f5}"
                ".box{background:#fff;padding:2rem;border-radius:12px;max-width:400px;margin:0 auto;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
                "</style></head><body><div class='box'><div style='font-size:4rem'>ðŸ”„</div>"
                "<h2>Restarting...</h2><p>Device will restart in a moment.</p>"
                "<p id='counter'>Redirecting in 5 seconds...</p></div>"
                "<script>var s=5;setInterval(function(){s--;if(s<=0)location.href='/dashboard';"
                "else document.getElementById('counter').textContent='Redirecting in '+s+' seconds...';},1000);</script>"
                "</body></html>");
        }
        
        shouldRestart = true;
        restartTimer = millis();
    });
    
    // ========== DEVICE SETTINGS ==========
    server.on("/settings_device", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Device", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“±"));
            out.printf("%s</h1>", "Device");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Device");
            out.print(F("</div>"));
            
            out.print(F("<form action='/save_device' method='POST'>"));
            
            // Identity card
            out.printf("<div class='card'><div class='card-header'>ðŸ“› %s</div><div class='card-body'>", "Identity");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='deviceName' value='%s' maxlength='32' class='form-input'></div>", "Device Name", config.deviceName);
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Device ID");
            out.print(F("<div style='display:flex;gap:8px;flex-wrap:wrap'>"));
            out.printf("<input type='text' id='devIdInput' name='deviceId' value='%s' maxlength='12' class='form-input' style='flex:1;min-width:120px' disabled>", config.deviceId);
            out.printf("<button type='button' onclick='regenId()' class='btn btn-secondary btn-sm'>ðŸ”„ %s</button>", "New ID");
            out.print(F("<button type='button' onclick='toggleManualId()' class='btn btn-secondary btn-sm'>âœï¸ Manual</button>"));
            out.print(F("</div></div>"));
            out.print(F("</div></div>"));

            // Storage View Preference
            out.printf("<div class='card'><div class='card-header'>ðŸ’¾ %s</div><div class='card-body'>", "Storage");
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Default Storage");
            out.print(F("<select name='defaultStorageView' class='form-input form-select'"));
            if (!sdAvailable) out.print(F(" disabled style='background-color:#eee;color:#999'"));
            out.print(F(">"));
            out.printf("<option value='0' %s>ðŸ’¾ %s (LittleFS)</option>", 
                config.hardware.defaultStorageView == 0 ? "selected" : "", "Internal");
            out.printf("<option value='1' %s>ðŸ’³ %s</option>", 
                config.hardware.defaultStorageView == 1 ? "selected" : "", "SD Card");
            out.print(F("</select></div></div></div>"));
            
            // Web Server card
            out.printf("<div class='card'><div class='card-header'>ðŸŒ %s</div><div class='card-body'>", "Web Server");
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='forceWebServer' value='1' %s> %s</label></div>", config.forceWebServer ? "checked" : "", "Force Web Server On");
            out.print(F("</div></div>"));
            
            // System Info card
            out.printf("<div class='card'><div class='card-header'>ðŸ“Š %s</div><div class='card-body'>", "System Info");
            out.print(F("<div class='form-row'>"));
            out.printf("<div><strong>%s</strong><div class='text-primary'>%s</div></div>", "Firmware", getVersionString().c_str());
            out.printf("<div><strong>%s</strong><div>%d</div></div>", "Boot Count", bootCount);
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Mode", getModeDisplay().c_str());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Free Heap", formatFileSize(ESP.getFreeHeap()).c_str());
            out.print(F("</div>"));
            out.print(F("<div class='form-row mt-1'>"));
            out.printf("<div><strong>CPU</strong><div>%d MHz</div></div>", getCpuFrequencyMhz());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Chip", ESP.getChipModel());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Flash", formatFileSize(ESP.getFlashChipSize()).c_str());
            out.printf("<div><strong>SDK</strong><div>%s</div></div>", ESP.getSdkVersion());
            out.print(F("</div></div></div>"));
            
            // Changelog card - loads from /changelog.txt (LittleFS)
            out.print(F("<div class='card'><div class='card-header' onclick=\"var el=document.getElementById('changelog');el.classList.toggle('hidden');if(!el.dataset.loaded)loadChangelog();\" style='cursor:pointer'>"));
            out.printf("ðŸ“œ %s <small class='text-muted'>(tap)</small></div>", "Change Log");
            out.print(F("<div class='card-body hidden' id='changelog' data-loaded=''>"));
            out.printf("<div class='text-muted'>%s...</div>", "Loading");
            out.print(F("</div></div>"));
            
            // About card
            out.print(F("<div class='card'><div class='card-header'>â„¹ï¸ About</div><div class='card-body'>"));
            out.print(F("<div style='text-align:center'>"));
            out.print(F("<div style='font-size:1.2rem;font-weight:bold;color:var(--primary);margin-bottom:0.5rem'>Water Logger</div>"));
            out.print(F("<div style='color:var(--text-muted);font-size:0.9rem'>Created by</div>"));
            out.print(F("<div style='font-weight:600;margin:0.5rem 0'>Petko Georgiev</div>"));
            out.print(F("<div style='color:var(--text-muted);font-size:0.85rem;margin-top:0.25rem'>ðŸ“ Sevlievo, Bulgaria</div>"));
            out.print(F("</div></div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>ðŸ’¾ Save</button></form>"));
            out.print(F("<script>"));
            out.print(F("function loadChangelog(){"));
            out.print(F("var el=document.getElementById('changelog');"));
            out.print(F("fetch('/api/changelog').then(r=>r.ok?r.text():Promise.reject('File not found')).then(txt=>{"));
            out.print(F("var html='';var lines=txt.trim().split('\\n');var inVersion=false;"));
            out.print(F("lines.forEach(function(line){"));
            out.print(F("line=line.trim();if(!line)return;"));
            out.print(F("if(line.startsWith('##')){"));
            out.print(F("if(inVersion)html+='</ul></div>';"));
            out.print(F("var ver=line.substring(2).trim();"));
            out.print(F("var isCurrent=ver.indexOf('Current')>=0||lines.indexOf(line)<3;"));
            out.print(F("html+='<div style=\"margin-top:0.5rem;padding:0.5rem;'+(isCurrent?'background:var(--primary);color:#fff':'background:var(--bg)')+';border-radius:4px\">';"));
            out.print(F("html+='<strong>'+ver+'</strong><ul style=\"margin:0.5rem 0 0 1rem;padding:0;font-size:0.9rem\">';"));
            out.print(F("inVersion=true;"));
            out.print(F("}else if(line.startsWith('-')&&inVersion){"));
            out.print(F("html+='<li>'+line.substring(1).trim()+'</li>';"));
            out.print(F("}});"));
            out.print(F("if(inVersion)html+='</ul></div>';"));
            out.print(F("el.innerHTML=html;el.dataset.loaded='1';"));
            out.print(F("}).catch(e=>{el.innerHTML='<div class=\"alert alert-warning\">Changelog not found. Upload /changelog.txt</div>';el.dataset.loaded='1';});"));
            out.print(F("}"));
            out.print(R"raw(
            function regenId() {
                if(confirm('Generate new ID based on MAC address?')) {
                    fetch('/api/regen-id', {method: 'POST'})
                    .then(response => response.text())
                    .then(newId => {
                        document.getElementById('devIdInput').value = newId;
                        document.getElementById('devIdInput').disabled = false;
                        alert('New ID generated: ' + newId + '. Click Save to apply.');
                    })
                    .catch(err => alert('Error generating ID: ' + err));
                }
            }
            function toggleManualId() {
                var inp = document.getElementById('devIdInput');
                if(inp.disabled) {
                    inp.disabled = false;
                    inp.focus();
                    inp.select();
                } else {
                    inp.disabled = true;
                }
            }
            )raw");
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_device", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("deviceName", true))
            strncpy(config.deviceName, r->getParam("deviceName", true)->value().c_str(), 32);
        if (r->hasParam("deviceId", true)) {
            String newId = r->getParam("deviceId", true)->value();
            if (newId.length() > 0 && newId.length() <= 12) {
                strncpy(config.deviceId, newId.c_str(), sizeof(config.deviceId) - 1);
            }
        }
        config.forceWebServer = r->hasParam("forceWebServer", true);
        if (r->hasParam("defaultStorageView", true)) {
            config.hardware.defaultStorageView = r->getParam("defaultStorageView", true)->value().toInt();
        }
        saveConfig();
        r->redirect("/settings_device");
    });
    
    // API endpoint for generating new Device ID from MAC address
    server.on("/api/regen-id", HTTP_POST, [](AsyncWebServerRequest *request) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        // Use last 8 characters (4 bytes) of MAC
        String newId = mac.substring(mac.length() - 8);
        newId.toUpperCase();
        request->send(200, "text/plain", newId);
    });
    
    // ========== FLOW METER SETTINGS ==========
    server.on("/settings_flowmeter", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Flow Meter", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ’§"));
            out.printf("%s</h1>", "Flow Meter");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Flow Meter");
            
            out.print(F("<form action='/save_flowmeter' method='POST'>"));
            
            // Calibration card
            out.printf("<div class='card'><div class='card-header'>ðŸ“ %s</div><div class='card-body'>", "Calibration");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' step='0.1' name='pulsesPerLiter' value='%.1f' class='form-input'></div>", "Pulses/Liter", config.flowMeter.pulsesPerLiter);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' step='0.01' name='calibrationMultiplier' value='%.2f' class='form-input'></div>", "Multiplier", config.flowMeter.calibrationMultiplier);
            out.print(F("</div></div></div>"));
            
            // Timing card
            out.printf("<div class='card'><div class='card-header'>â±ï¸ %s</div><div class='card-body'>", "Timing");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='monitoringWindowSecs' value='%d' class='form-input'>", "Window (sec)", config.flowMeter.monitoringWindowSecs);
            out.print(F("<p class='form-hint'>Time window of idle before logging.</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='firstLoopWindowSecs' value='%d' class='form-input'>", "First Loop (sec)", config.flowMeter.firstLoopMonitoringWindowSecs);
            out.print(F("<p class='form-hint'>First loop delay before logging (for delayed fill).</p></div>"));
            out.print(F("</div></div></div>"));
            
            // Test Mode card
            out.printf("<div class='card'><div class='card-header'>ðŸ§ª %s</div><div class='card-body'>", "Test Mode");
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='testMode' value='1' %s> %s</label>", config.flowMeter.testMode ? "checked" : "", "Enable");
            out.print(F("<p class='form-hint'>Activate WiFi Pin as LED output for visual check.<br>Solid light = IDLE / Blink = Water running.</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s (ms)</label><input type='number' name='blinkDuration' value='%d' class='form-input'></div>", "Blink Duration", config.flowMeter.blinkDuration);
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-top:1rem'>"));
            out.printf("<span>%s: <strong>%d</strong></span>", "Boot Count", bootCount);
            out.print(F("<button type='submit' name='resetBootCount' value='1' class='btn btn-sm btn-danger'>ðŸ”„ Reset</button>"));
            out.print(F("</div></div></div>"));
            
            out.printf("<button type='submit' class='btn btn-primary btn-block'>ðŸ’¾ %s</button></form>", "Save");
        });
    });
    
    server.on("/save_flowmeter", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("pulsesPerLiter", true))
            config.flowMeter.pulsesPerLiter = r->getParam("pulsesPerLiter", true)->value().toFloat();
        if (r->hasParam("calibrationMultiplier", true))
            config.flowMeter.calibrationMultiplier = r->getParam("calibrationMultiplier", true)->value().toFloat();
        if (r->hasParam("monitoringWindowSecs", true))
            config.flowMeter.monitoringWindowSecs = r->getParam("monitoringWindowSecs", true)->value().toInt();
        if (r->hasParam("firstLoopWindowSecs", true))
            config.flowMeter.firstLoopMonitoringWindowSecs = r->getParam("firstLoopWindowSecs", true)->value().toInt();
        config.flowMeter.testMode = r->hasParam("testMode", true);
        if (r->hasParam("blinkDuration", true))
            config.flowMeter.blinkDuration = r->getParam("blinkDuration", true)->value().toInt();
        if (r->hasParam("resetBootCount", true)) {
            bootCount = 0;
            backupBootCount();
        }
        saveConfig();
        r->redirect("/settings_flowmeter");
    });
    
    // ========== HARDWARE SETTINGS ==========
    server.on("/settings_hardware", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Hardware", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ”§"));
            out.printf("%s</h1>", "Hardware");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Hardware");
            
            out.printf("<div class='alert alert-warning'>âš ï¸ %s</div>", "Changes require device restart to take effect");
            
            // Board diagram card
            if (strlen(config.theme.boardDiagramPath) > 0) {
                out.print(F("<div class='card'><div class='card-body text-center'>"));
                out.printf("<img src='%s' style='max-width:100%%;height:auto' onerror=\"this.parentElement.parentElement.style.display='none'\">", config.theme.boardDiagramPath);
                out.print(F("</div></div>"));
            }
            
            out.print(F("<form action='/save_hardware' method='POST'>"));
            
            // Storage card
            out.printf("<div class='card'><div class='card-header'>ðŸ’¾ %s</div><div class='card-body'>", "Storage");
            out.print(F("<div class='form-group'><select name='storageType' id='storageType' class='form-input form-select' onchange='toggleSD()'>"));
            out.printf("<option value='0' %s>LittleFS (%s)</option>", config.hardware.storageType == STORAGE_LITTLEFS ? "selected" : "", "Internal");
            out.printf("<option value='1' %s>%s (SPI)</option>", config.hardware.storageType == STORAGE_SD_CARD ? "selected" : "", "SD Card");
            out.print(F("</select></div>"));
            
            String sdDisplay = config.hardware.storageType == STORAGE_SD_CARD ? "block" : "none";
            out.printf("<div id='sdPins' style='display:%s;margin-top:1rem'>", sdDisplay.c_str());
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>CS</label><input type='number' name='pinSdCS' value='%d' class='form-input'></div>", config.hardware.pinSdCS);
            out.printf("<div class='form-group'><label class='form-label'>MOSI</label><input type='number' name='pinSdMOSI' value='%d' class='form-input'></div>", config.hardware.pinSdMOSI);
            out.printf("<div class='form-group'><label class='form-label'>MISO</label><input type='number' name='pinSdMISO' value='%d' class='form-input'></div>", config.hardware.pinSdMISO);
            out.printf("<div class='form-group'><label class='form-label'>SCK</label><input type='number' name='pinSdSCK' value='%d' class='form-input'></div>", config.hardware.pinSdSCK);
            out.print(F("</div></div></div></div>"));
            
            // Wake-up card
            out.printf("<div class='card'><div class='card-header'>ðŸ˜´ %s</div><div class='card-body'>", "Wakeup Mode");
            out.print(F("<div class='form-group'><label class='form-label'>Button Active Level</label><select name='wakeupMode' class='form-input form-select'>"));
            out.printf("<option value='0' %s>Active HIGH (VCC)</option>", config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH ? "selected" : "");
            out.printf("<option value='1' %s>Active LOW (GND)</option>", config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_LOW ? "selected" : "");
            out.print(F("</select><p class='form-hint'>HIGH = button connects to VCC, LOW = button connects to GND</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>Debounce (ms)</label><input type='number' name='debounceMs' value='%d' min='20' max='500' class='form-input'>", config.hardware.debounceMs);
            out.print(F("<p class='form-hint'>Higher values reduce false triggers but slow response</p></div>"));
            out.print(F("</div></div>"));
            
            // Button & Sensor Pins card
            out.printf("<div class='card'><div class='card-header'>ðŸ”˜ %s</div><div class='card-body'>", "Pin Configuration");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:1rem'>GPIO pin numbers for ESP32-C3</p>"));
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWifiTrigger' value='%d' class='form-input'></div>", "WiFi Trigger", config.hardware.pinWifiTrigger);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWakeupFF' value='%d' class='form-input'></div>", "Full Flush Btn", config.hardware.pinWakeupFF);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWakeupPF' value='%d' class='form-input'></div>", "Part Flush Btn", config.hardware.pinWakeupPF);
            out.printf("<div class='form-group'><label class='form-label'>Flow</label><input type='number' name='pinFlowSensor' value='%d' class='form-input'></div>", config.hardware.pinFlowSensor);
            out.print(F("</div></div></div>"));
            
            // RTC Pins card
            out.print(F("<div class='card'><div class='card-header'>ðŸ• RTC DS1302</div><div class='card-body'>"));
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>CE</label><input type='number' name='pinRtcCE' value='%d' class='form-input'></div>", config.hardware.pinRtcCE);
            out.printf("<div class='form-group'><label class='form-label'>IO</label><input type='number' name='pinRtcIO' value='%d' class='form-input'></div>", config.hardware.pinRtcIO);
            out.printf("<div class='form-group'><label class='form-label'>CLK</label><input type='number' name='pinRtcSCLK' value='%d' class='form-input'></div>", config.hardware.pinRtcSCLK);
            out.print(F("</div></div></div>"));
            
            // CPU card
            out.printf("<div class='card'><div class='card-header'>âš¡ %s</div><div class='card-body'>", "CPU Frequency");
            out.print(F("<div class='form-group'><select name='cpuFreqMHz' class='form-input form-select'>"));
            out.printf("<option value='80' %s>80 MHz</option>", config.hardware.cpuFreqMHz == 80 ? "selected" : "");
            out.printf("<option value='160' %s>160 MHz</option>", config.hardware.cpuFreqMHz == 160 ? "selected" : "");
            out.print(F("</select></div></div></div>"));
            
            out.printf("<button type='submit' class='btn btn-primary btn-block' onclick='return confirm(\"Settings will be saved and device will restart. Continue?\")'>ðŸ’¾ %s</button></form>", "Save & Restart");
            
            out.print(F("<script>function toggleSD(){var e=document.getElementById('storageType'),d=document.getElementById('sdPins');d.style.display=e.value=='1'?'block':'none';}</script>"));
        });
    });
    
    server.on("/save_hardware", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Save all hardware settings
        if (r->hasParam("storageType", true))
            config.hardware.storageType = (StorageType)r->getParam("storageType", true)->value().toInt();
        if (r->hasParam("wakeupMode", true))
            config.hardware.wakeupMode = (WakeupMode)r->getParam("wakeupMode", true)->value().toInt();
        if (r->hasParam("pinWifiTrigger", true))
            config.hardware.pinWifiTrigger = r->getParam("pinWifiTrigger", true)->value().toInt();
        if (r->hasParam("pinWakeupFF", true))
            config.hardware.pinWakeupFF = r->getParam("pinWakeupFF", true)->value().toInt();
        if (r->hasParam("pinWakeupPF", true))
            config.hardware.pinWakeupPF = r->getParam("pinWakeupPF", true)->value().toInt();
        if (r->hasParam("pinFlowSensor", true))
            config.hardware.pinFlowSensor = r->getParam("pinFlowSensor", true)->value().toInt();
        if (r->hasParam("pinRtcCE", true))
            config.hardware.pinRtcCE = r->getParam("pinRtcCE", true)->value().toInt();
        if (r->hasParam("pinRtcIO", true))
            config.hardware.pinRtcIO = r->getParam("pinRtcIO", true)->value().toInt();
        if (r->hasParam("pinRtcSCLK", true))
            config.hardware.pinRtcSCLK = r->getParam("pinRtcSCLK", true)->value().toInt();
        if (r->hasParam("pinSdCS", true))
            config.hardware.pinSdCS = r->getParam("pinSdCS", true)->value().toInt();
        if (r->hasParam("pinSdMOSI", true))
            config.hardware.pinSdMOSI = r->getParam("pinSdMOSI", true)->value().toInt();
        if (r->hasParam("pinSdMISO", true))
            config.hardware.pinSdMISO = r->getParam("pinSdMISO", true)->value().toInt();
        if (r->hasParam("pinSdSCK", true))
            config.hardware.pinSdSCK = r->getParam("pinSdSCK", true)->value().toInt();
        if (r->hasParam("cpuFreqMHz", true))
            config.hardware.cpuFreqMHz = r->getParam("cpuFreqMHz", true)->value().toInt();
        if (r->hasParam("debounceMs", true))
            config.hardware.debounceMs = constrain(r->getParam("debounceMs", true)->value().toInt(), 20, 500);
        saveConfig();
        
        sendRestartPage(r, "Device is restarting with new hardware settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });

    // ========== THEME SETTINGS ==========
    server.on("/settings_theme", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Theme", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸŽ¨"));
            out.printf("%s</h1>", "Theme");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Theme");
            
            out.print(F("<form action='/save_theme' method='POST'>"));
            
            // Mode card
            out.printf("<div class='card'><div class='card-header'>ðŸŒ“ %s</div><div class='card-body'>", "Mode");
            out.print(F("<div class='form-group'><select name='themeMode' class='form-input form-select'>"));
            out.printf("<option value='0' %s>â˜€ï¸ %s</option>", config.theme.mode == THEME_LIGHT ? "selected" : "", "Light");
            out.printf("<option value='1' %s>ðŸŒ™ %s</option>", config.theme.mode == THEME_DARK ? "selected" : "", "Dark");
            out.printf("<option value='2' %s>ðŸ”„ %s</option>", config.theme.mode == THEME_AUTO ? "selected" : "", "Auto");
            out.print(F("</select></div>"));
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='showIcons' value='1' %s> %s</label></div>", config.theme.showIcons ? "checked" : "", "Show icons");
            out.print(F("</div></div>"));
            
            // Colors card
            out.printf("<div class='card'><div class='card-header'>ðŸŽ¨ %s</div><div class='card-body'>", "Colors");
            out.print(F("<div class='color-grid'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='primaryColor' value='%s' class='form-input'></div>", "Primary", config.theme.primaryColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='secondaryColor' value='%s' class='form-input'></div>", "Secondary", config.theme.secondaryColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='bgColor' value='%s' class='form-input'></div>", "Background", config.theme.bgColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='textColor' value='%s' class='form-input'></div>", "Text", config.theme.textColor);
            out.print(F("</div>"));
            
            out.print(F("<h4 class='mt-2'>Chart</h4>"));
            out.print(F("<div class='color-grid' style='grid-template-columns:repeat(3,1fr)'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='ffColor' value='%s' class='form-input'></div>", "Full Flush Btn", config.theme.ffColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='pfColor' value='%s' class='form-input'></div>", "Part Flush Btn", config.theme.pfColor);
            out.printf("<div class='form-group'><label class='form-label'>Other</label><input type='color' name='otherColor' value='%s' class='form-input'></div>", config.theme.otherColor);
            out.print(F("</div>"));
            
            out.printf("<h4 class='mt-2'>%s</h4>", "Storage");
            out.print(F("<div class='color-grid'>"));
            out.printf("<div class='form-group'><label class='form-label'>Normal</label><input type='color' name='storageBarColor' value='%s' class='form-input'></div>", config.theme.storageBarColor);
            out.printf("<div class='form-group'><label class='form-label'>70%%+</label><input type='color' name='storageBar70Color' value='%s' class='form-input'></div>", config.theme.storageBar70Color);
            out.printf("<div class='form-group'><label class='form-label'>90%%+</label><input type='color' name='storageBar90Color' value='%s' class='form-input'></div>", config.theme.storageBar90Color);
            out.printf("<div class='form-group'><label class='form-label'>Border</label><input type='color' name='storageBarBorder' value='%s' class='form-input'></div>", config.theme.storageBarBorder);
            out.print(F("</div></div></div>"));
            
            // Images card
            out.printf("<div class='card'><div class='card-header'>ðŸ–¼ï¸ %s</div><div class='card-body'>", "Images");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:1rem'>Supported: PNG, JPG, GIF, SVG, ICO. Path: /file.png or /folder/file.png</p>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='logoSource' value='%s' placeholder='/logo.png' class='form-input'></div>", "Logo Path", config.theme.logoSource);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='faviconPath' value='%s' placeholder='/favicon.ico' class='form-input'></div>", "Favicon", config.theme.faviconPath);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='boardDiagramPath' value='%s' placeholder='/board.png' class='form-input'></div>", "Board Diagram", config.theme.boardDiagramPath);
            out.print(F("</div></div>"));
            
            // Chart.js card
            out.printf("<div class='card'><div class='card-header'>ðŸ“ˆ %s</div><div class='card-body'>", "Chart Source");
            out.print(F("<div class='form-group'><label class='form-label'>Chart.js Library</label><select name='chartSource' id='chartSource' class='form-input form-select' onchange='toggleChartPath()'>"));
            out.printf("<option value='0' %s>ðŸ“ %s</option>", config.theme.chartSource == CHART_LOCAL ? "selected" : "", "Local File");
            out.printf("<option value='1' %s>ðŸŒ %s</option>", config.theme.chartSource == CHART_CDN ? "selected" : "", "CDN (Online)");
            out.print(F("</select><p class='form-hint'>Local: requires /chart.min.js file, CDN: needs internet</p></div>"));
            String localDisplay = config.theme.chartSource == CHART_LOCAL ? "block" : "none";
            out.printf("<div id='chartLocalPath' style='display:%s'>", localDisplay.c_str());
            out.printf("<div class='form-group'><label class='form-label'>Local Path</label><input type='text' name='chartLocalPath' value='%s' placeholder='/chart.min.js' class='form-input'></div>", config.theme.chartLocalPath);
            out.print(F("</div>"));
            out.print(F("<script>function toggleChartPath(){document.getElementById('chartLocalPath').style.display=document.getElementById('chartSource').value=='0'?'block':'none';}</script>"));
            
            // Dashboard column label format
            out.print(F("<div class='form-group mt-1'><label class='form-label'>Dashboard Column Labels</label>"));
            out.print(F("<select name='chartLabelFormat' class='form-input form-select'>"));
            out.printf("<option value='0' %s>ðŸ“… Date + Time</option>", config.theme.chartLabelFormat == LABEL_DATETIME ? "selected" : "");
            out.printf("<option value='1' %s>ðŸ”¢ Boot Count (#1234)</option>", config.theme.chartLabelFormat == LABEL_BOOTCOUNT ? "selected" : "");
            out.printf("<option value='2' %s>ðŸ“…ðŸ”¢ Both (Date Time #Boot)</option>", config.theme.chartLabelFormat == LABEL_BOTH ? "selected" : "");
            out.print(F("</select><p class='form-hint'>How to label bars in dashboard chart</p></div>"));
            out.print(F("</div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>ðŸ’¾ Save</button></form>"));
        });
    });
    
    server.on("/save_theme", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("themeMode", true))
            config.theme.mode = (ThemeMode)r->getParam("themeMode", true)->value().toInt();
        config.theme.showIcons = r->hasParam("showIcons", true);
        if (r->hasParam("primaryColor", true))
            strncpy(config.theme.primaryColor, r->getParam("primaryColor", true)->value().c_str(), 7);
        if (r->hasParam("secondaryColor", true))
            strncpy(config.theme.secondaryColor, r->getParam("secondaryColor", true)->value().c_str(), 7);
        if (r->hasParam("bgColor", true))
            strncpy(config.theme.bgColor, r->getParam("bgColor", true)->value().c_str(), 7);
        if (r->hasParam("textColor", true))
            strncpy(config.theme.textColor, r->getParam("textColor", true)->value().c_str(), 7);
        if (r->hasParam("ffColor", true))
            strncpy(config.theme.ffColor, r->getParam("ffColor", true)->value().c_str(), 7);
        if (r->hasParam("pfColor", true))
            strncpy(config.theme.pfColor, r->getParam("pfColor", true)->value().c_str(), 7);
        if (r->hasParam("otherColor", true))
            strncpy(config.theme.otherColor, r->getParam("otherColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBarColor", true))
            strncpy(config.theme.storageBarColor, r->getParam("storageBarColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBar70Color", true))
            strncpy(config.theme.storageBar70Color, r->getParam("storageBar70Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBar90Color", true))
            strncpy(config.theme.storageBar90Color, r->getParam("storageBar90Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBarBorder", true))
            strncpy(config.theme.storageBarBorder, r->getParam("storageBarBorder", true)->value().c_str(), 7);
        if (r->hasParam("logoSource", true))
            strncpy(config.theme.logoSource, r->getParam("logoSource", true)->value().c_str(), 128);
        if (r->hasParam("faviconPath", true))
            strncpy(config.theme.faviconPath, r->getParam("faviconPath", true)->value().c_str(), 32);
        if (r->hasParam("boardDiagramPath", true))
            strncpy(config.theme.boardDiagramPath, r->getParam("boardDiagramPath", true)->value().c_str(), 64);
        if (r->hasParam("chartSource", true))
            config.theme.chartSource = (ChartSource)r->getParam("chartSource", true)->value().toInt();
        if (r->hasParam("chartLocalPath", true))
            strncpy(config.theme.chartLocalPath, r->getParam("chartLocalPath", true)->value().c_str(), 64);
        if (r->hasParam("chartLabelFormat", true))
            config.theme.chartLabelFormat = (ChartLabelFormat)r->getParam("chartLabelFormat", true)->value().toInt();
        
        saveConfig();
        r->redirect("/settings_theme");
    });
    
    // ========== TIME SYNC SETTINGS ==========
    server.on("/settings_time", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Time", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ•"));
            out.printf("%s</h1>", "Time Sync");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Time Sync");
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            // RTC Status card
            out.printf("<div class='card'><div class='card-header'>ðŸ“Ÿ %s</div><div class='card-body'>", "RTC Status");
            RtcDateTime now;
            bool timeValid = false;
            if (Rtc) {
                now = Rtc->GetDateTime();
                timeValid = (now.Year() >= 2020 && now.Year() <= 2100 && now.Month() >= 1 && now.Day() >= 1);
            }
            
            if (!Rtc) {
                out.printf("<div class='alert alert-error'>âŒ RTC %s</div>", "Error");
            } else if (!timeValid) {
                out.printf("<div class='alert alert-warning'>âš ï¸ %s</div>", "Warning");
            } else {
                out.printf("<div class='alert alert-success'>âœ… %s</div>", "Success");
            }
            
            out.print(F("<div class='text-center' style='font-size:2.5rem;font-family:monospace;font-weight:bold;margin:1rem 0'>"));
            out.print(getRtcDateTimeString());
            out.print(F("</div>"));
            
            if (Rtc) {
                out.print(F("<p class='text-muted text-center' style='font-size:0.8rem'>"));
                out.printf("Protected: %s | Running: %s",
                    Rtc->GetIsWriteProtected() ? "Yes" : "No",
                    Rtc->GetIsRunning() ? "Yes" : "No");
                out.print(F("</p>"));
            }
            out.print(F("</div></div>"));
            
            // Manual Set card
            out.printf("<div class='card card-accent' style='border-top-color:#27ae60'><div class='card-header'>â° %s</div><div class='card-body'>", "Set Time");
            out.print(F("<form action='/set_time' method='POST'>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><label class='form-label'>Date</label><input type='date' name='date' required class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='time' name='time' required class='form-input'></div>", "Time");
            out.printf("</div><button type='submit' class='btn btn-success btn-block'>â° %s</button></form>", "Set Time");
            out.print(F("</div></div>"));
            
            // NTP Sync card
            out.printf("<div class='card'><div class='card-header'>ðŸŒ %s</div><div class='card-body'>", "NTP Sync");
            
            if (wifiConnectedAsClient) {
                out.printf("<div class='alert alert-success'>âœ… %s</div>", "Connected");
                out.print(F("<form action='/sync_time' method='POST' style='margin-bottom:1rem'>"));
                out.printf("<button type='submit' class='btn btn-primary'>ðŸ”„ %s</button></form>", "NTP Sync");
            } else {
                out.printf("<div class='alert alert-warning'>âš ï¸ %s</div>", "Disconnected");
            }
            
            out.print(F("<form action='/save_time' method='POST'>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><label class='form-label'>NTP Server</label><input type='text' name='ntpServer' value='"));
            out.print(config.network.ntpServer);
            out.print(F("' class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s (UTC+)</label><input type='number' name='timezone' value='%d' min='-12' max='14' class='form-input'></div>", "Timezone", config.network.timezone);
            out.print(F("</div>"));
            out.printf("<button type='submit' class='btn btn-secondary'>ðŸ’¾ %s</button>", "Save");
            out.print(F("</form></div></div>"));
            
            // RTC Options card
            out.print(F("<div class='card'><div class='card-header'>âš™ï¸ RTC & System Options</div><div class='card-body'>"));
            
            // Write Protection
            if (Rtc) {
                out.print(F("<form action='/rtc_protect' method='POST' style='margin-bottom:1rem'>"));
                out.print(F("<div class='form-group'><label class='form-label'>"));
                out.printf("<input type='checkbox' name='protect' value='1' %s onchange='this.form.submit()'> ", 
                    Rtc->GetIsWriteProtected() ? "checked" : "");
                out.print(F("RTC Write Protected</label>"));
                out.print(F("<p class='form-hint'>When enabled, prevents accidental time changes</p>"));
                out.print(F("</div></form>"));
            }
            
            // Flush Log Buffer
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem;padding:0.75rem;background:var(--bg);border-radius:8px'>"));
            out.print(F("<div><strong>Log Buffer</strong><p class='form-hint' style='margin:0'>Write pending logs to storage</p></div>"));
            out.print(F("<form action='/flush_logs' method='POST'><button type='submit' class='btn btn-secondary'>ðŸ’¾ Flush</button></form>"));
            out.print(F("</div>"));
            
            // Boot Count section
            out.print(F("<div style='padding:0.75rem;background:var(--bg);border-radius:8px'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:0.75rem'>"));
            out.printf("<div><strong>Boot Count:</strong> <span style='font-size:1.25rem;color:var(--primary)'>%d</span></div>", bootCount);
            
            // Read backup value
            uint32_t backupValue = 0;
            File bf = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "r");
            if (bf) { bf.read((uint8_t*)&backupValue, sizeof(backupValue)); bf.close(); }
            out.printf("<div><strong>Backup:</strong> <span style='font-size:1.25rem;color:var(--secondary)'>%d</span></div>", backupValue);
            out.print(F("</div>"));
            
            out.print(F("<div style='display:flex;gap:0.5rem;flex-wrap:wrap'>"));
            out.print(F("<form action='/backup_bootcount' method='POST'><button type='submit' class='btn btn-secondary'>ðŸ“¤ Backup Now</button></form>"));
            out.print(F("<form action='/restore_bootcount' method='POST' onsubmit='return confirm(\"Restore boot count from backup?\")'><button type='submit' class='btn btn-warning'>ðŸ“¥ Restore</button></form>"));
            out.print(F("</div>"));
            out.print(F("<p class='form-hint' style='margin-top:0.5rem'>Backup saves to RTC RAM + flash file</p>"));
            out.print(F("</div>"));
            
            out.print(F("</div></div>"));
        });
    });
    
    server.on("/save_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("ntpServer", true))
            strncpy(config.network.ntpServer, r->getParam("ntpServer", true)->value().c_str(), 64);
        if (r->hasParam("timezone", true))
            config.network.timezone = r->getParam("timezone", true)->value().toInt();
        saveConfig();
        r->redirect("/settings_time");
    });
    
    server.on("/sync_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (syncTimeFromNTP()) {
            rtcValid = true;
            statusMessage = "<div class='alert alert-success'>âœ… Time synced successfully!</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Sync failed - check network connection</div>";
        }
        r->redirect("/settings_time");
    });
    
    server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Block time setting during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            statusMessage = "<div class='alert alert-warning'>âš ï¸ Cannot set time during active measurement</div>";
            r->redirect("/settings_time");
            return;
        }
        
        if (r->hasParam("date", true) && r->hasParam("time", true) && Rtc) {
            String dateStr = r->getParam("date", true)->value();  // YYYY-MM-DD
            String timeStr = r->getParam("time", true)->value();  // HH:MM
            
            int year = dateStr.substring(0, 4).toInt();
            int month = dateStr.substring(5, 7).toInt();
            int day = dateStr.substring(8, 10).toInt();
            int hour = timeStr.substring(0, 2).toInt();
            int minute = timeStr.substring(3, 5).toInt();
            
            Serial.printf("Setting RTC: %04d-%02d-%02d %02d:%02d:00\n", year, month, day, hour, minute);
            
            RtcDateTime dt(year, month, day, hour, minute, 0);
            bool success = false;
            
            // Multiple attempts to set time
            for (int attempt = 0; attempt < 3 && !success; attempt++) {
                Rtc->SetIsWriteProtected(false);
                delay(10);
                Rtc->SetIsRunning(true);
                delay(10);
                Rtc->SetDateTime(dt);
                delay(100);
                
                // Verify
                RtcDateTime verify = Rtc->GetDateTime();
                Serial.printf("Attempt %d: Read back %04d-%02d-%02d\n", 
                    attempt + 1, verify.Year(), verify.Month(), verify.Day());
                
                if (verify.Year() == year && verify.Month() == month && verify.Day() == day) {
                    success = true;
                    rtcValid = true;
                }
            }
            
            if (success) {
                statusMessage = "<div class='alert alert-success'>âœ… Time set successfully!</div>";
            } else {
                statusMessage = "<div class='alert alert-error'>âŒ Failed to set time - check RTC connection</div>";
            }
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Invalid date/time or RTC not available</div>";
        }
        r->redirect("/settings_time");
    });
    
    // RTC Write Protection toggle
    server.on("/rtc_protect", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (Rtc) {
            bool protect = r->hasParam("protect", true);
            Rtc->SetIsWriteProtected(protect);
            statusMessage = protect ? 
                "<div class='alert alert-success'>ðŸ”’ RTC write protection enabled</div>" :
                "<div class='alert alert-success'>ðŸ”“ RTC write protection disabled</div>";
        }
        r->redirect("/settings_time");
    });
    
    // Flush log buffer
    server.on("/flush_logs", HTTP_POST, [](AsyncWebServerRequest *r) {
        flushLogBufferToFS();
        statusMessage = "<div class='alert alert-success'>âœ… Log buffer flushed to storage</div>";
        r->redirect("/settings_time");
    });
    
    // Backup boot count
    server.on("/backup_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        backupBootCount();
        statusMessage = "<div class='alert alert-success'>âœ… Boot count backed up</div>";
        r->redirect("/settings_time");
    });
    
    // Restore boot count from backup
    server.on("/restore_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        uint32_t oldValue = bootCount;
        restoreBootCount();
        char msg[128];
        snprintf(msg, sizeof(msg), "<div class='alert alert-success'>âœ… Boot count restored: %d â†’ %d</div>", oldValue, bootCount);
        statusMessage = String(msg);
        r->redirect("/settings_time");
    });
    
    // ========== NETWORK SETTINGS ==========
    server.on("/settings_network", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Network", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸŒ"));
            out.printf("%s</h1>", "Network");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Network");
            
            // Status card
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem'>"));
            out.print(F("<span><strong>Status:</strong> "));
            if (wifiConnectedAsClient) {
                out.printf("%s: %s", "Connected", connectedSSID.c_str());
            } else {
                out.printf("%s", "AP Mode");
            }
            out.print(F("</span>"));
            out.print(F("<span><strong>IP:</strong> "));
            out.print(currentIPAddress);
            out.print(F("</span></div></div></div>"));
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            out.print(F("<form action='/save_network' method='POST'>"));
            
            // WiFi Mode card
            out.printf("<div class='card'><div class='card-header'>ðŸ“¡ %s</div><div class='card-body'>", "WiFi Mode");
            out.print(F("<div class='form-group'><select name='wifiMode' id='wifiMode' class='form-input form-select' onchange='toggleMode()'>"));
            out.printf("<option value='0' %s>ðŸ“¶ %s (AP)</option>", config.network.wifiMode == WIFIMODE_AP ? "selected" : "", "AP Mode");
            out.printf("<option value='1' %s>ðŸ”— %s</option>", config.network.wifiMode == WIFIMODE_CLIENT ? "selected" : "", "Client Mode");
            out.print(F("</select></div></div></div>"));
            
            // AP Settings card
            String apDisplay = config.network.wifiMode == WIFIMODE_AP ? "block" : "none";
            out.printf("<div id='apSection' style='display:%s'>", apDisplay.c_str());
            out.printf("<div class='card'><div class='card-header'>ðŸ“¶ %s</div><div class='card-body'>", "AP Mode");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='apSSID' value='%s' placeholder='WaterLogger' class='form-input'></div>", "SSID", config.network.apSSID);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "Password");
            out.printf("<input type='password' name='apPassword' id='apPass' value='%s' class='form-input' style='flex:1'>", config.network.apPassword);
            out.print(F("<button type='button' class='btn btn-secondary' onclick=\"togglePass('apPass')\">ðŸ‘ï¸</button></div></div>"));
            out.print(F("</div></div>"));
            
            // AP Network Properties card
            out.print(F("<div class='card'><div class='card-header'>ðŸ”§ Network Properties</div><div class='card-body'>"));
            
            // Current AP info (read-only)
            if (!wifiConnectedAsClient) {
                out.print(F("<div class='mb-1' style='padding:10px;background:var(--bg);border-radius:var(--radius-sm);font-size:0.9rem'>"));
                out.printf("<strong>Current:</strong> IP: %s | ", WiFi.softAPIP().toString().c_str());
                out.printf("Clients: %d", WiFi.softAPgetStationNum());
                out.print(F("</div>"));
            }
            
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='apIP' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apIP[0], config.network.apIP[1], config.network.apIP[2], config.network.apIP[3]);
            out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='apGateway' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apGateway[0], config.network.apGateway[1], config.network.apGateway[2], config.network.apGateway[3]);
            out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='apSubnet' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apSubnet[0], config.network.apSubnet[1], config.network.apSubnet[2], config.network.apSubnet[3]);
            out.print(F("</div></div></div>"));
            out.print(F("</div>"));
            
            // Client Settings card
            String clientDisplay = config.network.wifiMode == WIFIMODE_CLIENT ? "block" : "none";
            out.printf("<div id='clientSection' style='display:%s'>", clientDisplay.c_str());
            out.printf("<div class='card'><div class='card-header'>ðŸ”— %s</div><div class='card-body'>", "Client Mode");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "SSID");
            out.printf("<input type='text' name='clientSSID' id='clientSSID' value='%s' class='form-input' style='flex:1'>", config.network.clientSSID);
            out.print(F("<button type='button' class='btn btn-secondary' onclick='scanWifi()'>ðŸ”</button></div></div>"));
            out.print(F("<div id='wifiList' style='display:none;margin-bottom:1rem;max-height:200px;overflow-y:auto;border:1px solid var(--border);border-radius:8px'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "Password");
            out.printf("<input type='password' name='clientPassword' id='clientPass' value='%s' class='form-input' style='flex:1'>", config.network.clientPassword);
            out.print(F("<button type='button' class='btn btn-secondary' onclick=\"togglePass('clientPass')\">ðŸ‘ï¸</button></div></div>"));
            out.print(F("</div></div>"));
            
            // Client Network Properties card
            out.print(F("<div class='card'><div class='card-header'>ðŸ”§ Network Properties</div><div class='card-body'>"));
            
            // IP fields - show current values when connected, otherwise show static config
            out.print(F("<div class='form-row' id='staticFields'>"));
            if (wifiConnectedAsClient && !config.network.useStaticIP) {
                // Show current DHCP values
                out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='staticIP' id='fldIP' value='%s' class='form-input'></div>",
                    WiFi.localIP().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='gateway' id='fldGW' value='%s' class='form-input'></div>",
                    WiFi.gatewayIP().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='subnet' id='fldSN' value='%s' class='form-input'></div>",
                    WiFi.subnetMask().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>DNS Server</label><input type='text' name='dns' id='fldDNS' value='%s' class='form-input'></div>",
                    WiFi.dnsIP().toString().c_str());
            } else {
                // Show static config values
                out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='staticIP' id='fldIP' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.staticIP[0], config.network.staticIP[1], config.network.staticIP[2], config.network.staticIP[3]);
                out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='gateway' id='fldGW' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.gateway[0], config.network.gateway[1], config.network.gateway[2], config.network.gateway[3]);
                out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='subnet' id='fldSN' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.subnet[0], config.network.subnet[1], config.network.subnet[2], config.network.subnet[3]);
                out.printf("<div class='form-group'><label class='form-label'>DNS Server</label><input type='text' name='dns' id='fldDNS' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.dns[0], config.network.dns[1], config.network.dns[2], config.network.dns[3]);
            }
            out.print(F("</div>"));
            
            // Static IP checkbox below fields
            out.printf("<div class='form-group mt-1'><label class='form-label'><input type='checkbox' name='useStaticIP' id='staticCheck' value='1' %s onchange='toggleStatic()'> Use Static IP (enable editing)</label></div>", 
                config.network.useStaticIP ? "checked" : "");
            
            out.print(F("</div></div>"));
            out.print(F("</div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>ðŸ’¾ Save & Restart</button></form>"));
            
            // JavaScript
            out.print(F("<script>"));
            out.print(F("function toggleMode(){var m=document.getElementById('wifiMode').value;"));
            out.print(F("document.getElementById('apSection').style.display=m=='0'?'block':'none';"));
            out.print(F("document.getElementById('clientSection').style.display=m=='1'?'block':'none';}"));
            out.print(F("function togglePass(id){var e=document.getElementById(id);e.type=e.type=='password'?'text':'password';}"));
            out.print(F("function toggleStatic(){"));
            out.print(F("var en=document.getElementById('staticCheck').checked;"));
            out.print(F("['fldIP','fldGW','fldSN','fldDNS'].forEach(function(id){"));
            out.print(F("var el=document.getElementById(id);"));
            out.print(F("el.disabled=!en;"));
            out.print(F("el.style.opacity=en?'1':'0.5';"));
            out.print(F("el.style.cursor=en?'text':'not-allowed';"));
            out.print(F("});"));
            out.print(F("}toggleStatic();"));  // Call on page load
            out.print(F("var scanRetries=0;"));
            out.print(F("function scanWifi(){"));
            out.print(F("var l=document.getElementById('wifiList');l.innerHTML='<div class=\"list-item\">ðŸ” Scanning...</div>';l.style.display='block';scanRetries=0;"));
            out.print(F("fetch('/wifi_scan_start').then(()=>setTimeout(checkScanResult,2000));}"));
            out.print(F("function checkScanResult(){"));
            out.print(F("fetch('/wifi_scan_result').then(r=>r.json()).then(d=>{"));
            out.print(F("var l=document.getElementById('wifiList');"));
            out.print(F("if(d.scanning){scanRetries++;if(scanRetries<10){l.innerHTML='<div class=\"list-item\">ðŸ” Scanning... ('+scanRetries+')</div>';setTimeout(checkScanResult,1000);}else{l.innerHTML='<div class=\"list-item\">â±ï¸ Scan timeout</div>';}}"));
            out.print(F("else if(d.error){l.innerHTML='<div class=\"list-item\">âŒ '+d.error+'</div>';}"));
            out.print(F("else if(d.networks.length==0){l.innerHTML='<div class=\"list-item\">ðŸ“¡ No networks found</div>';}"));
            out.print(F("else{var h='';d.networks.forEach(n=>{h+='<div class=\"list-item\" style=\"cursor:pointer\" onclick=\"selectWifi(\\''+n.ssid.replace(/'/g,\"\\\\'\")+'\\')\">';"));
            out.print(F("h+=(n.secure?'ðŸ”’':'ðŸ“¶')+' '+n.ssid+' <small class=\"text-muted\">('+n.rssi+' dBm)</small></div>';});l.innerHTML=h;}"));
            out.print(F("}).catch(e=>{document.getElementById('wifiList').innerHTML='<div class=\"list-item\">âŒ Error: '+e+'</div>';});}"));
            out.print(F("function selectWifi(ssid){document.getElementById('clientSSID').value=ssid;document.getElementById('wifiList').style.display='none';}"));
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_network", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("wifiMode", true))
            config.network.wifiMode = (WiFiModeType)r->getParam("wifiMode", true)->value().toInt();
        if (r->hasParam("apSSID", true))
            strncpy(config.network.apSSID, r->getParam("apSSID", true)->value().c_str(), 32);
        if (r->hasParam("apPassword", true))
            strncpy(config.network.apPassword, r->getParam("apPassword", true)->value().c_str(), 64);
        if (r->hasParam("clientSSID", true))
            strncpy(config.network.clientSSID, r->getParam("clientSSID", true)->value().c_str(), 32);
        if (r->hasParam("clientPassword", true))
            strncpy(config.network.clientPassword, r->getParam("clientPassword", true)->value().c_str(), 64);
        config.network.useStaticIP = r->hasParam("useStaticIP", true);
        
        // Parse Static IP settings
        if (r->hasParam("staticIP", true)) {
            String ip = r->getParam("staticIP", true)->value();
            sscanf(ip.c_str(), "%hhu.%hhu.%hhu.%hhu", 
                &config.network.staticIP[0], &config.network.staticIP[1],
                &config.network.staticIP[2], &config.network.staticIP[3]);
        }
        if (r->hasParam("gateway", true)) {
            String gw = r->getParam("gateway", true)->value();
            sscanf(gw.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.gateway[0], &config.network.gateway[1],
                &config.network.gateway[2], &config.network.gateway[3]);
        }
        if (r->hasParam("subnet", true)) {
            String sn = r->getParam("subnet", true)->value();
            sscanf(sn.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.subnet[0], &config.network.subnet[1],
                &config.network.subnet[2], &config.network.subnet[3]);
        }
        if (r->hasParam("dns", true)) {
            String dns = r->getParam("dns", true)->value();
            sscanf(dns.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.dns[0], &config.network.dns[1],
                &config.network.dns[2], &config.network.dns[3]);
        }
        
        // Parse AP IP settings
        if (r->hasParam("apIP", true)) {
            String ip = r->getParam("apIP", true)->value();
            sscanf(ip.c_str(), "%hhu.%hhu.%hhu.%hhu", 
                &config.network.apIP[0], &config.network.apIP[1],
                &config.network.apIP[2], &config.network.apIP[3]);
        }
        if (r->hasParam("apGateway", true)) {
            String gw = r->getParam("apGateway", true)->value();
            sscanf(gw.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.apGateway[0], &config.network.apGateway[1],
                &config.network.apGateway[2], &config.network.apGateway[3]);
        }
        if (r->hasParam("apSubnet", true)) {
            String sn = r->getParam("apSubnet", true)->value();
            sscanf(sn.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.apSubnet[0], &config.network.apSubnet[1],
                &config.network.apSubnet[2], &config.network.apSubnet[3]);
        }
        
        saveConfig();
        
        sendRestartPage(r, "Device is restarting with new network settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });
    
    // ========== DATALOG SETTINGS ==========
    server.on("/settings_datalog", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Data Log", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“"));
            out.printf("%s</h1>", "Data Log");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Data Log");
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            out.print(F("<form action='/save_datalog' method='POST'>"));
            
            // Active Log File card
            out.printf("<div class='card'><div class='card-header'>ðŸ“„ %s</div><div class='card-body'>", "Current File");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><select name='currentFile' class='form-input form-select'>", "Select file");
            out.print(generateDatalogFileOptions());
            out.print(F("</select></div>"));
            
            out.print(F("<div class='form-row mt-1'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='prefix' value='", "Prefix");
            out.print(config.datalog.prefix);
            out.print(F("' placeholder='datalog' class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='folder' value='%s' placeholder='/' class='form-input'>", "Folder", config.datalog.folder);
            out.print(F("<p class='text-muted' style='font-size:.75rem;margin-top:2px'>Leave empty for root, or /logs for subfolder</p></div>"));
            out.print(F("</div>"));
            out.printf("<button type='submit' name='action' value='create' class='btn btn-success'>âž• %s</button>", "Create");
            out.print(F("</div></div>"));
            
            // Rotation settings card
            out.printf("<div class='card'><div class='card-header'>ðŸ”„ %s</div><div class='card-body'>", "Rotation");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:0.75rem'>How to split log files over time</p>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><select name='rotation' id='rotation' class='form-input form-select' onchange='toggleMaxSize()'>"));
            out.printf("<option value='0' %s>%s</option>", config.datalog.rotation == ROTATION_NONE ? "selected" : "", "None");
            out.printf("<option value='1' %s>%s</option>", config.datalog.rotation == ROTATION_DAILY ? "selected" : "", "Daily");
            out.printf("<option value='2' %s>%s</option>", config.datalog.rotation == ROTATION_WEEKLY ? "selected" : "", "Weekly");
            out.printf("<option value='3' %s>%s</option>", config.datalog.rotation == ROTATION_MONTHLY ? "selected" : "", "Monthly");
            out.printf("<option value='4' %s>By Size</option>", config.datalog.rotation == ROTATION_SIZE ? "selected" : "");
            out.print(F("</select></div>"));
            String maxSizeDisplay = config.datalog.rotation == ROTATION_SIZE ? "block" : "none";
            out.printf("<div class='form-group' id='maxSizeGroup' style='display:%s'><label class='form-label'>%s (KB)</label><input type='number' name='maxSizeKB' value='%d' min='10' max='10000' class='form-input'></div>", maxSizeDisplay.c_str(), "Max Size", config.datalog.maxSizeKB);
            out.print(F("</div>"));
            
            out.print(F("<div class='form-group'><label class='form-label'><input type='checkbox' name='timestampFilename' value='1' "));
            out.print(config.datalog.timestampFilename ? "checked" : "");
            out.print(F("> Timestamp filename</label></div>"));
            
            out.print(F("<div class='form-group'><label class='form-label'><input type='checkbox' name='includeDeviceId' value='1' "));
            out.print(config.datalog.includeDeviceId ? "checked" : "");
            out.print(F("> Device ID in filename</label></div>"));
            out.print(F("</div></div>"));
            
            // Format Configuration card
            out.print(F("<div class='card'><div class='card-header'>âš™ï¸ Log Format</div><div class='card-body'>"));
            out.print(F("<div style='display:flex;flex-wrap:wrap;gap:8px;align-items:flex-end'>"));
            
            // Date format
            out.print(F("<div class='form-group' style='min-width:100px'><label class='form-label'>Date</label>"));
            out.print(F("<select name='dateFormat' id='dfDate' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>Off</option>", config.datalog.dateFormat == DATE_OFF ? "selected" : "");
            out.printf("<option value='1' %s>DD/MM/YYYY</option>", config.datalog.dateFormat == DATE_DDMMYYYY ? "selected" : "");
            out.printf("<option value='2' %s>MM/DD/YYYY</option>", config.datalog.dateFormat == DATE_MMDDYYYY ? "selected" : "");
            out.printf("<option value='3' %s>YYYY-MM-DD</option>", config.datalog.dateFormat == DATE_YYYYMMDD ? "selected" : "");
            out.printf("<option value='4' %s>DD.MM.YYYY</option>", config.datalog.dateFormat == DATE_DDMMYYYY_DOT ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Time format
            out.print(F("<div class='form-group' style='min-width:90px'><label class='form-label'>Start</label>"));
            out.print(F("<select name='timeFormat' id='dfTime' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>HH:MM:SS</option>", config.datalog.timeFormat == TIME_HHMMSS ? "selected" : "");
            out.printf("<option value='1' %s>HH:MM</option>", config.datalog.timeFormat == TIME_HHMM ? "selected" : "");
            out.printf("<option value='2' %s>12h AM/PM</option>", config.datalog.timeFormat == TIME_12H ? "selected" : "");
            out.print(F("</select></div>"));
            
            // End format
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>End</label>"));
            out.print(F("<select name='endFormat' id='dfEnd' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>Time</option>", config.datalog.endFormat == END_TIME ? "selected" : "");
            out.printf("<option value='1' %s>Duration</option>", config.datalog.endFormat == END_DURATION ? "selected" : "");
            out.printf("<option value='2' %s>Off</option>", config.datalog.endFormat == END_OFF ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Boot count
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>Boot#</label>"));
            out.print(F("<select name='includeBootCount' id='dfBoot' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='1' %s>On</option>", config.datalog.includeBootCount ? "selected" : "");
            out.printf("<option value='0' %s>Off</option>", !config.datalog.includeBootCount ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Trigger (always on, just info)
            out.print(F("<div class='form-group' style='min-width:60px'><label class='form-label'>Trigger</label>"));
            out.print(F("<div style='padding:10px 12px;border-radius:var(--radius-sm);background:var(--border);color:var(--text-muted);text-align:center;font-size:1rem'>On</div></div>"));
            
            // Volume format
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>Volume</label>"));
            out.print(F("<select name='volumeFormat' id='dfVol' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>L:0,00</option>", config.datalog.volumeFormat == VOL_L_COMMA ? "selected" : "");
            out.printf("<option value='1' %s>L:0.00</option>", config.datalog.volumeFormat == VOL_L_DOT ? "selected" : "");
            out.printf("<option value='2' %s>0.00</option>", config.datalog.volumeFormat == VOL_NUM_ONLY ? "selected" : "");
            out.printf("<option value='3' %s>Off</option>", config.datalog.volumeFormat == VOL_OFF ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Extra presses
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>FF/PF</label>"));
            out.print(F("<select name='includeExtraPresses' id='dfExtra' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='1' %s>On</option>", config.datalog.includeExtraPresses ? "selected" : "");
            out.printf("<option value='0' %s>Off</option>", !config.datalog.includeExtraPresses ? "selected" : "");
            out.print(F("</select></div>"));
            
            out.print(F("</div>"));
            
            // Preview line
            out.print(F("<div class='mt-1'><label class='form-label'>Preview:</label>"));
            out.print(F("<div id='logPreview' style='font-family:monospace;font-size:0.85rem;padding:10px 12px;border-radius:var(--radius-sm);background:var(--bg);color:var(--text);border:1px solid var(--border);overflow-x:auto;white-space:nowrap'></div>"));
            out.print(F("</div></div></div>"));
            
            // Post-Correction card
            out.print(F("<div class='card'><div class='card-header'>ðŸ”„ Post-Correction</div><div class='card-body'>"));
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='postCorrectionEnabled' value='1' %s "
                "onchange=\"document.getElementById('pcFields').style.display=this.checked?'block':'none'\"> Enable Post-Correction</label></div>",
                config.datalog.postCorrectionEnabled ? "checked" : "");
            out.printf("<div id='pcFields' style='display:%s'>", config.datalog.postCorrectionEnabled ? "block" : "none");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>PF â†’ FF Threshold (L)</label>"
                "<input type='number' step='0.1' name='pfToFfThreshold' value='%.1f' min='0.1' max='20' class='form-input'>"
                "<p class='form-hint'>PF_BTN with volume â‰¥ this â†’ corrected to FF_BTN</p></div>", config.datalog.pfToFfThreshold);
            out.printf("<div class='form-group'><label class='form-label'>FF â†’ PF Threshold (L)</label>"
                "<input type='number' step='0.1' name='ffToPfThreshold' value='%.1f' min='0.1' max='20' class='form-input'>"
                "<p class='form-hint'>FF_BTN with volume â‰¤ this â†’ corrected to PF_BTN</p></div>", config.datalog.ffToPfThreshold);
            out.print(F("</div>"));
            out.printf("<div class='form-group'><label class='form-label'>Button Hold Threshold (ms)</label>"
                "<input type='number' step='50' name='manualPressThresholdMs' value='%d' min='0' max='5000' class='form-input'>"
                "<p class='form-hint'>If button is held longer than this, correction is skipped (extended hold = intentional). 0 = disabled.</p></div>",
                config.datalog.manualPressThresholdMs);
            out.print(F("<p class='form-hint' style='margin-top:0.5rem'>Corrects button identification by comparing measured water volume against thresholds. "
                "Also checks how long the button was held â€” an extended press releases more water intentionally, so correction is skipped. "
                "Only applies to clean cycles with no extra button presses. Corrections are logged to btn_log.txt.</p>"));
            out.print(F("</div></div></div>"));
            
            // Available files card
            out.printf("<div class='card'><div class='card-header'>ðŸ“Š %s</div><div class='card-body' style='padding:0'>", "Files");
            if (fsAvailable && activeFS) {
                int fileCount = 0;
                std::function<void(const String&)> listFiles = [&](const String& path) {
                    File dir = activeFS->open(path);
                    if (!dir || !dir.isDirectory()) return;
                    while (File entry = dir.openNextFile()) {
                        String name = String(entry.name());
                        String fullPath = path == "/" ? "/" + name : path + "/" + name;
                        if (entry.isDirectory()) {
                            listFiles(fullPath);
                        } else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) {
                            fileCount++;
                            bool isCurrent = (fullPath == String(config.datalog.currentFile));
                            out.print(F("<div class='list-item'>"));
                            out.print(F("<span>"));
                            if (isCurrent) out.print(F("<strong class='text-success'>âœ“ "));
                            out.print(fullPath);
                            out.print(F(" <small class='text-muted'>("));
                            out.print(formatFileSize(entry.size()));
                            out.print(F(")</small>"));
                            if (isCurrent) out.print(F("</strong>"));
                            out.print(F("</span><span class='btn-group'>"));
                            out.printf("<a href='/download?file=%s' class='btn btn-sm btn-secondary'>ðŸ“¥</a>", fullPath.c_str());
                            if (!isCurrent) {
                                String storage = (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) ? "sdcard" : "internal";
                                out.printf("<a href='/delete?path=%s&storage=%s&return=datalog' class='btn btn-sm btn-danger' onclick='return confirm(\"Delete?\")'>ðŸ—‘ï¸</a>", fullPath.c_str(), storage.c_str());
                            }
                            out.print(F("</span></div>"));
                        }
                        entry.close();
                    }
                    dir.close();
                };
                listFiles("/");
                if (fileCount == 0) out.printf("<div class='list-item text-muted'>%s</div>", "No log files");
            }
            out.print(F("</div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>ðŸ’¾ Save</button></form>"));
            out.print(F("<script>function toggleMaxSize(){document.getElementById('maxSizeGroup').style.display=document.getElementById('rotation').value==='4'?'block':'none';}"));
            // Preview generator
            out.print(F("function updatePreview(){var p=[];var d=new Date();"));
            out.print(F("var df=document.getElementById('dfDate').value;"));
            out.print(F("var tf=document.getElementById('dfTime').value;"));
            out.print(F("var ef=document.getElementById('dfEnd').value;"));
            out.print(F("var dd=d.getDate().toString().padStart(2,'0'),mm=(d.getMonth()+1).toString().padStart(2,'0'),yy=d.getFullYear();"));
            out.print(F("var hh=d.getHours().toString().padStart(2,'0'),mi=d.getMinutes().toString().padStart(2,'0'),ss=d.getSeconds().toString().padStart(2,'0');"));
            // Date
            out.print(F("if(df==='1')p.push(dd+'/'+mm+'/'+yy);"));
            out.print(F("else if(df==='2')p.push(mm+'/'+dd+'/'+yy);"));
            out.print(F("else if(df==='3')p.push(yy+'-'+mm+'-'+dd);"));
            out.print(F("else if(df==='4')p.push(dd+'.'+mm+'.'+yy);"));
            // Start time
            out.print(F("var tStr='';if(tf==='0')tStr=hh+':'+mi+':'+ss;"));
            out.print(F("else if(tf==='1')tStr=hh+':'+mi;"));
            out.print(F("else{var h=d.getHours()%12||12;tStr=h+':'+mi+':'+ss+(d.getHours()<12?'AM':'PM');}"));
            out.print(F("p.push(tStr);"));
            // End
            out.print(F("if(ef==='0')p.push(tStr);"));
            out.print(F("else if(ef==='1')p.push('45s');"));
            // Boot
            out.print(F("if(document.getElementById('dfBoot').value==='1')p.push('#:1234');"));
            // Trigger
            out.print(F("p.push('FF_BTN');"));
            // Volume
            out.print(F("var vf=document.getElementById('dfVol').value;"));
            out.print(F("if(vf==='0')p.push('L:2,50');"));
            out.print(F("else if(vf==='1')p.push('L:2.50');"));
            out.print(F("else if(vf==='2')p.push('2.50');"));
            // Extra
            out.print(F("if(document.getElementById('dfExtra').value==='1'){p.push('FF0');p.push('PF1');}"));
            out.print(F("document.getElementById('logPreview').textContent=p.join('|');}"));
            out.print(F("updatePreview();"));
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_datalog", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Save all settings first
        if (r->hasParam("currentFile", true))
            strncpy(config.datalog.currentFile, r->getParam("currentFile", true)->value().c_str(), 64);
        if (r->hasParam("prefix", true))
            strncpy(config.datalog.prefix, r->getParam("prefix", true)->value().c_str(), 32);
        if (r->hasParam("folder", true))
            strncpy(config.datalog.folder, r->getParam("folder", true)->value().c_str(), 32);
        if (r->hasParam("rotation", true))
            config.datalog.rotation = (DatalogRotation)r->getParam("rotation", true)->value().toInt();
        if (r->hasParam("maxSizeKB", true))
            config.datalog.maxSizeKB = r->getParam("maxSizeKB", true)->value().toInt();
        config.datalog.timestampFilename = r->hasParam("timestampFilename", true);
        config.datalog.includeDeviceId = r->hasParam("includeDeviceId", true);
        
        // Format settings
        if (r->hasParam("dateFormat", true))
            config.datalog.dateFormat = r->getParam("dateFormat", true)->value().toInt();
        if (r->hasParam("timeFormat", true))
            config.datalog.timeFormat = r->getParam("timeFormat", true)->value().toInt();
        if (r->hasParam("endFormat", true))
            config.datalog.endFormat = r->getParam("endFormat", true)->value().toInt();
        if (r->hasParam("volumeFormat", true))
            config.datalog.volumeFormat = r->getParam("volumeFormat", true)->value().toInt();
        config.datalog.includeBootCount = r->hasParam("includeBootCount", true) && r->getParam("includeBootCount", true)->value() == "1";
        config.datalog.includeExtraPresses = r->hasParam("includeExtraPresses", true) && r->getParam("includeExtraPresses", true)->value() == "1";
        // Post-correction settings
        config.datalog.postCorrectionEnabled = r->hasParam("postCorrectionEnabled", true);
        if (r->hasParam("pfToFfThreshold", true))
            config.datalog.pfToFfThreshold = r->getParam("pfToFfThreshold", true)->value().toFloat();
        if (r->hasParam("ffToPfThreshold", true))
            config.datalog.ffToPfThreshold = r->getParam("ffToPfThreshold", true)->value().toFloat();
        if (r->hasParam("manualPressThresholdMs", true))
            config.datalog.manualPressThresholdMs = r->getParam("manualPressThresholdMs", true)->value().toInt();
        
        saveConfig();
        
        // Check if we should create a new file
        String action = r->hasParam("action", true) ? r->getParam("action", true)->value() : "";
        
        if (action == "create" && fsAvailable && activeFS) {
            // Generate filename based on saved settings
            String folder = String(config.datalog.folder);
            if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
            if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
            if (folder.length() == 0) folder = "/";
            
            // Create folder if needed
            if (folder != "/" && !activeFS->exists(folder)) {
                activeFS->mkdir(folder);
            }
            
            // Generate new filename
            String newFile = folder + String(config.datalog.prefix);
            
            // Add device ID if enabled
            if (config.datalog.includeDeviceId && strlen(config.deviceId) > 0) {
                newFile += "_";
                newFile += config.deviceId;
            }
            
            // Add timestamp ONLY if enabled
            if (config.datalog.timestampFilename) {
                if (Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    char buf[20];
                    snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
                        now.Year(), now.Month(), now.Day(),
                        now.Hour(), now.Minute(), now.Second());
                    newFile += buf;
                } else {
                    newFile += "_";
                    newFile += String(millis());
                }
            }
            
            newFile += ".txt";
            
            // Create empty file
            File f = activeFS->open(newFile, "w");
            if (f) {
                f.close();
                strncpy(config.datalog.currentFile, newFile.c_str(), 64);
                saveConfig();
                statusMessage = "<div class='alert alert-success'>âœ… Created: " + newFile + "</div>";
            } else {
                statusMessage = "<div class='alert alert-error'>âŒ Failed to create file</div>";
            }
        } else if (action != "create") {
            statusMessage = "<div class='alert alert-success'>âœ… " + String("Saved") + "</div>";
        }
        
        r->redirect("/settings_datalog");
    });

    // ========== LIVE MONITOR ==========
    server.on("/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Live", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“¡"));
            out.printf("%s</h1>", "Live");
            out.printf("<div class='breadcrumb'><span id='connStatus' class='text-success'>â— %s</span></div>", "Connected");
            out.print(F("</div>"));
            
            // Time card with Mode
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:flex-start'>"));
            // Mode on left
            out.printf("<div><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Mode");
            out.print(F("<div id='mode' style='font-weight:bold;font-size:1.1rem'>"));
            if (onlineLoggerMode) {
                out.printf("ðŸŒ %s", "Online Logger");
            } else if (apModeTriggered) {
                out.printf("ðŸ“¶ %s", "Web Only");
            } else {
                out.printf("ðŸ“Š %s", "Logging");
            }
            out.print(F("</div></div>"));
            // Time in center
            out.print(F("<div class='text-center' style='flex:1'>"));
            out.print(F("<div id='time' style='font-size:2.5rem;font-family:monospace;font-weight:bold'>--:--:--</div>"));
            out.print(F("</div>"));
            // Empty right for balance
            out.print(F("<div style='width:100px'></div>"));
            out.print(F("</div></div></div>"));
            
            // Current Cycle card
            out.printf("<div class='card'><div class='card-header'>ðŸ’§ %s</div><div class='card-body'>", "Current Cycle");
            out.print(F("<div class='stats-grid'>"));
            out.printf("<div class='stat-card'><div class='value' id='liters'>0.00</div><div class='label'>%s</div></div>", "Liters");
            out.printf("<div class='stat-card'><div class='value' id='pulses'>0</div><div class='label'>%s</div></div>", "Pulses");
            out.printf("<div class='stat-card'><div class='value' id='ffCount' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.ffColor, "Extra FF");
            out.printf("<div class='stat-card'><div class='value' id='pfCount' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.pfColor, "Extra PF");
            out.print(F("</div>"));
            out.print(F("<div class='mt-1' style='display:flex;justify-content:space-between;font-size:0.85rem'>"));
            out.printf("<span>%s: <strong id='trigger'>-</strong></span>", "Trigger");
            out.printf("<span>%s: <strong id='cycleTime'>0</strong>s</span>", "Duration");
            out.print(F("</div></div></div>"));
            
            // State Machine card
            out.printf("<div class='card'><div class='card-header'>âš™ï¸ %s</div><div class='card-body'>", "State Machine");
            out.print(F("<div style='display:flex;align-items:center;justify-content:center;gap:1.5rem;flex-wrap:wrap'>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "State");
            out.print(F("<div id='state' style='font-weight:bold;padding:8px 16px;border-radius:6px;background:#e0e0e0;font-size:1.1rem'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Timer");
            out.print(F("<div id='stateRemaining' style='font-size:2rem;font-weight:bold;color:var(--primary)'>-</div></div>"));
            out.print(F("</div>"));
            out.print(F("<div class='mt-1 text-muted' style='font-size:0.75rem;text-align:center'>"));
            out.printf("ðŸ”µ IDLE â†’ ðŸŸ¡ WAIT_FLOW (%ds) â†’ ðŸŸ¢ MONITORING (%ds idle) â†’ Logging", 
                config.flowMeter.firstLoopMonitoringWindowSecs, config.flowMeter.monitoringWindowSecs);
            out.print(F("</div></div></div>"));
            
            // Button States card
            out.printf("<div class='card'><div class='card-header'>ðŸ”˜ %s</div><div class='card-body'>", "Button States");
            out.print(F("<div style='display:flex;justify-content:space-around;flex-wrap:wrap;gap:1rem'>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Full Flush Btn");
            out.print(F("<div id='ff' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Part Flush Btn");
            out.print(F("<div id='pf' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "WiFi Trigger");
            out.print(F("<div id='wifi' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.print(F("</div></div></div>"));
            
            // Recent Logs card
            out.printf("<div class='card'><div class='card-header'>ðŸ“ %s</div>", "Recent Logs");
            out.printf("<div class='card-body' style='padding:0;overflow-x:auto'><div id='recentLogs' style='font-family:monospace;font-size:0.8rem'>%s...</div></div></div>", "Loading");
            
            // System card (text only - no stat cards)
            out.printf("<div class='card'><div class='card-header'>ðŸ“Š %s</div><div class='card-body'>", "System");
            // Row 1: Boot, Uptime, Chip
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem'>"));
            out.printf("<span>%s: <strong id='boot'>0</strong></span>", "Boot Count");
            out.printf("<span>%s: <strong id='uptime'>0</strong>s</span>", "Uptime");
            out.printf("<span>%s: <strong>%s</strong></span>", "Chip", ESP.getChipModel());
            out.print(F("</div>"));
            // Row 2: RAM, CPU, Storage
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem;margin-top:0.5rem'>"));
            out.printf("<span>RAM: <strong id='heap'>-</strong> / <strong id='heapTotal'>-</strong></span>");
            out.printf("<span>CPU: <strong>%d</strong> MHz</span>", getCpuFrequencyMhz());
            out.printf("<span>%s: <strong id='storage'>-</strong></span>", "Storage");
            out.print(F("</div>"));
            // Row 3: Network (left), empty (middle), IP (right)
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem;margin-top:0.5rem'>"));
            out.print(F("<span>"));
            out.print(getNetworkDisplay());
            out.print(F("</span>"));
            out.print(F("<span></span>"));  // Empty middle
            out.print(F("<span>IP: <strong>"));
            out.print(currentIPAddress);
            out.print(F("</strong></span>"));
            out.print(F("</div></div></div>"));
            
            // Hide footer in Live Monitor
            out.print(F("<style>.app-footer{display:none!important}</style>"));
            
            // JavaScript
            out.print(F("<script>"));
            out.print(F("function fmt(b){return b>1048576?(b/1048576).toFixed(1)+'MB':b>1024?(b/1024).toFixed(1)+'KB':b+'B'}"));
            out.print(F("var stateColors={IDLE:'#3498db',WAIT_FLOW:'#f39c12',MONITORING:'#27ae60',DONE:'#e74c3c'};"));
            out.print(F("function upd(){fetch('/api/live').then(r=>r.json()).then(d=>{"));
            out.print(F("document.getElementById('time').textContent=d.time;"));
            out.print(F("document.getElementById('trigger').textContent=d.trigger;"));
            out.print(F("document.getElementById('cycleTime').textContent=d.cycleTime;"));
            out.print(F("document.getElementById('pulses').textContent=d.pulses;"));
            out.print(F("document.getElementById('liters').textContent=d.liters.toFixed(2);"));
            out.print(F("document.getElementById('ffCount').textContent=d.ffCount;"));
            out.print(F("document.getElementById('pfCount').textContent=d.pfCount;"));
            out.print(F("var stEl=document.getElementById('state');stEl.textContent=d.state;stEl.style.background=stateColors[d.state]||'#95a5a6';stEl.style.color='#fff';"));
            out.print(F("var remEl=document.getElementById('stateRemaining');"));
            out.print(F("if(d.stateRemaining>=0){remEl.textContent=d.stateRemaining+'s';}else{remEl.textContent='-';}"));
            out.print(F("document.getElementById('ff').textContent=d.ff?'Pressed':'Released';"));
            out.print(F("document.getElementById('ff').style.background=d.ff?'#27ae60':'#95a5a6';"));
            out.print(F("document.getElementById('pf').textContent=d.pf?'Pressed':'Released';"));
            out.print(F("document.getElementById('pf').style.background=d.pf?'#27ae60':'#95a5a6';"));
            out.print(F("document.getElementById('wifi').textContent=d.wifi?'Pressed':'Released';"));
            out.print(F("document.getElementById('wifi').style.background=d.wifi?'#3498db':'#95a5a6';"));
            out.print(F("document.getElementById('boot').textContent=d.boot;"));
            out.print(F("document.getElementById('heap').textContent=fmt(d.heap);"));
            out.print(F("document.getElementById('heapTotal').textContent=fmt(d.heapTotal);"));
            out.print(F("document.getElementById('uptime').textContent=d.uptime;"));
            out.print(F("if(d.fsTotal)document.getElementById('storage').textContent=fmt(d.fsUsed)+'/'+fmt(d.fsTotal);"));
            out.print(F("var modeEl=document.getElementById('mode');"));
            out.printf("if(d.mode=='online')modeEl.innerHTML='ðŸŒ %s';", "Online Logger");
            out.printf("else if(d.mode=='webonly')modeEl.innerHTML='ðŸ“¶ %s';", "Web Only");
            out.printf("else modeEl.innerHTML='ðŸ“Š %s';", "Logging");
            out.printf("document.getElementById('connStatus').className='text-success';document.getElementById('connStatus').textContent='â— %s';", "Connected");
            out.printf("}).catch(()=>{document.getElementById('connStatus').className='text-danger';document.getElementById('connStatus').textContent='â— %s'})}",  "Disconnected");
            
            out.printf("var ffColor='%s',pfColor='%s',otherColor='%s';", 
                config.theme.ffColor, config.theme.pfColor, config.theme.otherColor);
            out.print(F("function hexToRgba(hex,a){var r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);return 'rgba('+r+','+g+','+b+','+a+')';}"));
            
            // JS translation strings for table headers
            out.printf("var lang={time:'%s',trigger:'%s',volume:'%s',noLogs:'%s'};", 
                "Time", "Trigger", "Volume", "Loading");
            
            // Recent logs with Extra FF/PF columns - translated headers
            out.print(F("function updLogs(){fetch('/api/recent_logs').then(r=>r.json()).then(d=>{"));
            out.print(F("var el=document.getElementById('recentLogs');"));
            out.print(F("if(d.logs&&d.logs.length>0){"));
            out.print(F("var html='<table style=\"width:100%;border-collapse:collapse;font-size:0.75rem\">';"));
            out.print(F("html+='<tr style=\"background:var(--bg)\"><th style=\"padding:6px;text-align:left\">'+lang.time+'</th><th>'+lang.trigger+'</th><th>'+lang.volume+'</th><th>+FF</th><th>+PF</th></tr>';"));
            out.print(F("d.logs.forEach(function(l){"));
            out.print(F("var bg=l.trigger.indexOf('FF')>=0?hexToRgba(ffColor,0.15):l.trigger.indexOf('PF')>=0?hexToRgba(pfColor,0.15):hexToRgba(otherColor,0.1);"));
            out.print(F("var tc=l.trigger.indexOf('FF')>=0?ffColor:l.trigger.indexOf('PF')>=0?pfColor:otherColor;"));
            out.print(F("html+='<tr style=\"background:'+bg+'\"><td style=\"padding:6px\">'+l.time+'</td><td style=\"color:'+tc+';font-weight:bold;text-align:center\">'+l.trigger+'</td><td style=\"text-align:center\">'+l.volume+'</td><td style=\"text-align:center\">'+l.ff+'</td><td style=\"text-align:center\">'+l.pf+'</td></tr>';"));
            out.print(F("});html+='</table>';el.innerHTML=html;"));
            out.print(F("}else{el.innerHTML='<div class=\"list-item text-muted\">No log entries yet</div>';}"));
            out.print(F("}).catch(()=>{})}"));
            
            out.print(F("setInterval(upd,500);upd();"));
            out.print(F("setInterval(updLogs,3000);updLogs();"));
            out.print(F("</script>"));
        });
    });
    
    // ========== LIVE API (JSON) ==========
    server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<1024> doc;
        
        // Atomic read of volatile ISR variables
        noInterrupts();
        uint32_t safePulses = pulseCount;
        interrupts();
        
        doc["time"] = getRtcDateTimeString();
        doc["ff"] = digitalRead(config.hardware.pinWakeupFF);
        doc["pf"] = digitalRead(config.hardware.pinWakeupPF);
        doc["wifi"] = digitalRead(config.hardware.pinWifiTrigger);
        doc["pulses"] = safePulses;
        doc["boot"] = bootCount;
        doc["heap"] = ESP.getFreeHeap();
        doc["heapTotal"] = ESP.getHeapSize();
        doc["uptime"] = millis() / 1000;
        
        // Current cycle info
        doc["trigger"] = cycleStartedBy;
        doc["cycleTime"] = (millis() - cycleStartTime) / 1000;
        doc["ffCount"] = highCountFF;
        doc["pfCount"] = highCountPF;
        doc["totalPulses"] = cycleTotalPulses + safePulses;
        
        // State machine info
        const char* stateNames[] = {"IDLE", "WAIT_FLOW", "MONITORING", "DONE"};
        doc["state"] = stateNames[loggingState];
        doc["stateTime"] = (millis() - stateStartTime) / 1000;
        
        // Remaining time in current state
        if (loggingState == STATE_WAIT_FLOW) {
            long remaining = (BUTTON_WAIT_FLOW_MS - (millis() - stateStartTime)) / 1000;
            doc["stateRemaining"] = remaining > 0 ? remaining : 0;
        } else if (loggingState == STATE_MONITORING && lastFlowPulseTime > 0) {
            long remaining = (FLOW_IDLE_TIMEOUT_MS - (millis() - lastFlowPulseTime)) / 1000;
            doc["stateRemaining"] = remaining > 0 ? remaining : 0;
        } else {
            doc["stateRemaining"] = -1;  // Not applicable
        }
        
        // Calculate current flow in liters
        float liters = 0;
        if (config.flowMeter.pulsesPerLiter > 0) {
            liters = (float)safePulses / config.flowMeter.pulsesPerLiter * config.flowMeter.calibrationMultiplier;
        }
        doc["liters"] = liters;
        
        // Mode info
        doc["mode"] = onlineLoggerMode ? "online" : (apModeTriggered ? "webonly" : "logging");
        
        uint64_t used, total; int percent;
        getStorageInfo(used, total, percent);
        doc["fsUsed"] = used;
        doc["fsTotal"] = total;
        
        sendJsonResponse(r, doc);
    });
    
    // ========== CHANGELOG API (always from LittleFS) ==========
    server.on("/api/changelog", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (LittleFS.exists("/changelog.txt")) {
            r->send(LittleFS, "/changelog.txt", "text/plain");
        } else {
            r->send(404, "text/plain", "Changelog not found");
        }
    });
    
    // ========== RECENT LOGS API ==========
    server.on("/api/recent_logs", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<2048> doc;
        JsonArray logs = doc.createNestedArray("logs");
        
        if (!fsAvailable || !activeFS) {
            doc["error"] = "Storage not available";
            sendJsonResponse(r, doc);
            return;
        }
        
        String logFile = getActiveDatalogFile();
        if (!activeFS->exists(logFile)) {
            doc["error"] = "Log file not found";
            sendJsonResponse(r, doc);
            return;
        }
        
        // Read last 5 lines from file
        File f = activeFS->open(logFile, "r");
        if (!f) {
            doc["error"] = "Cannot open file";
            sendJsonResponse(r, doc);
            return;
        }
        
        // Collect all lines (simple approach for small files)
        std::vector<String> lines;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                lines.push_back(line);
            }
        }
        f.close();
        
        // Get last 5 lines (newest first)
        int totalLines = (int)lines.size();
        int startIdx = totalLines > 5 ? totalLines - 5 : 0;
        for (int i = totalLines - 1; i >= startIdx; i--) {
            // Parse NEW format: DD/MM/YYYY|WakeTime|SleepTime|#:boot|REASON|L:vol|FFn|PFn
            // Also supports OLD format: DD/MM/YYYY|Time|#:boot|REASON|L:vol|FFn|PFn
            String line = lines[i];
            int p1 = line.indexOf('|');        // after date
            int p2 = line.indexOf('|', p1 + 1); // after wake time
            int p3 = line.indexOf('|', p2 + 1); // after sleep time OR boot count
            int p4 = line.indexOf('|', p3 + 1); // after boot count OR reason
            int p5 = line.indexOf('|', p4 + 1); // after reason OR volume
            int p6 = line.indexOf('|', p5 + 1); // after volume OR FF
            int p7 = line.indexOf('|', p6 + 1); // after FF (new format only)
            
            if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3) {
                JsonObject entry = logs.createNestedObject();
                
                // Check if this is new format (has sleep time) or old format
                // New format has 7 delimiters, old has 6
                bool isNewFormat = (p7 > p6);
                
                if (isNewFormat) {
                    // NEW format: Date|WakeTime|SleepTime|#:boot|REASON|L:vol|FFn|PFn
                    String wakeTime = line.substring(p1 + 1, p2);
                    String sleepTime = line.substring(p2 + 1, p3);
                    entry["time"] = line.substring(0, p1) + " " + wakeTime + "-" + sleepTime;
                    entry["trigger"] = line.substring(p4 + 1, p5);  // Reason
                    
                    String volStr = line.substring(p5 + 1, p6);
                    volStr.replace("L:", "");
                    volStr.replace(",", ".");
                    entry["volume"] = volStr + " L";
                    
                    String ffStr = line.substring(p6 + 1, p7);
                    ffStr.replace("FF", "");
                    entry["ff"] = ffStr.toInt();
                    
                    String pfStr = line.substring(p7 + 1);
                    pfStr.replace("PF", "");
                    entry["pf"] = pfStr.toInt();
                } else {
                    // OLD format: Date|Time|#:boot|REASON|L:vol|FFn|PFn
                    entry["time"] = line.substring(0, p2);  // Date + Time
                    entry["trigger"] = line.substring(p3 + 1, p4);  // Reason
                    
                    String volStr = line.substring(p4 + 1, p5);
                    volStr.replace("L:", "");
                    volStr.replace(",", ".");
                    entry["volume"] = volStr + " L";
                    
                    String ffStr = line.substring(p5 + 1, p6);
                    ffStr.replace("FF", "");
                    entry["ff"] = ffStr.toInt();
                    
                    String pfStr = line.substring(p6 + 1);
                    pfStr.replace("PF", "");
                    entry["pf"] = pfStr.toInt();
                }
            }
        }
        
        sendJsonResponse(r, doc);
    });
    
    // ========== STATISTICS (redirect to dashboard) ==========
    server.on("/statistics", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->redirect("/dashboard");
    });
    
    // Remove the old /api/stats endpoint since we're using direct file download now
    
    // ========== FILE OPERATIONS ==========
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("file")) {
            r->send(400, "text/plain", "No file");
            return;
        }
        String path = sanitizeFilename(r->getParam("file")->value());
        if (!path.startsWith("/")) path = "/" + path;
        
        // Check which storage to use
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = (storage == "sdcard" && sdAvailable) ? (fs::FS*)&SD : 
                        (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
        
        if (targetFS && targetFS->exists(path)) {
            // Extract filename from path
            String filename = path;
            int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                filename = path.substring(lastSlash + 1);
            }
            
            // Send with proper Content-Disposition header
            AsyncWebServerResponse *response = r->beginResponse(*targetFS, path, "application/octet-stream");
            response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            r->send(response);
        } else {
            r->send(404, "text/plain", "Not found");
        }
    });
    
    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path")) {
            r->redirect("/files");
            return;
        }
        String path = sanitizeFilename(r->getParam("path")->value());
        if (!path.startsWith("/")) path = "/" + path;
        
        // Get storage from URL or use current
        String storage = currentStorageView;
        if (r->hasParam("storage")) {
            storage = r->getParam("storage")->value();
        }
        
        // Determine which FS to use
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable) {
            targetFS = &SD;
        } else if (storage == "internal" && littleFsAvailable) {
            targetFS = &LittleFS;
        } else if (activeFS) {
            targetFS = activeFS;
        }
        
        bool deleted = false;
        if (targetFS) {
            File f = targetFS->open(path);
            if (f) {
                bool isDir = f.isDirectory();
                f.close();
                if (isDir) {
                    deleteRecursive(*targetFS, path);
                } else {
                    targetFS->remove(path);
                }
                deleted = true;
            }
        }
        
        // Get return directory from param or calculate from path
        String returnDir = r->hasParam("dir") ? r->getParam("dir")->value() : "";
        if (returnDir.isEmpty()) {
            int lastSlash = path.lastIndexOf('/');
            returnDir = lastSlash <= 0 ? "/" : path.substring(0, lastSlash);
        }
        
        // Check where to redirect
        String returnTo = r->hasParam("return") ? r->getParam("return")->value() : "";
        
        if (returnTo == "datalog") {
            statusMessage = deleted ? 
                "<div class='alert alert-success'>âœ… Deleted: " + path + "</div>" :
                "<div class='alert alert-error'>âŒ Failed to delete</div>";
            r->redirect("/settings_datalog");
        } else {
            statusMessage = deleted ? 
                "<div class='alert alert-success'>âœ… Deleted successfully</div>" :
                "<div class='alert alert-error'>âŒ Failed to delete</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
        }
    });
    
    // Move/Rename within same storage
    server.on("/move_file", HTTP_GET, [](AsyncWebServerRequest *r) {
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String srcPath = r->hasParam("src") ? sanitizePath(r->getParam("src")->value()) : "";
        String newName = r->hasParam("newName") ? r->getParam("newName")->value() : "";
        String destDir = r->hasParam("destDir") ? r->getParam("destDir")->value() : "";
        String returnDir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        
        if (srcPath.isEmpty() || newName.isEmpty()) {
            statusMessage = "<div class='alert alert-error'>âŒ Invalid parameters</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
            return;
        }
        
        // Determine FS
        fs::FS* fs = nullptr;
        if (storage == "sdcard" && sdAvailable) fs = &SD;
        else if (storage == "internal" && littleFsAvailable) fs = &LittleFS;
        
        if (!fs) {
            statusMessage = "<div class='alert alert-error'>âŒ Storage not available</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
            return;
        }
        
        // Build destination path
        String dstPath;
        if (destDir.isEmpty()) {
            // Same directory, just rename
            int lastSlash = srcPath.lastIndexOf('/');
            String srcDir = lastSlash <= 0 ? "/" : srcPath.substring(0, lastSlash);
            dstPath = buildPath(srcDir, newName);
        } else {
            dstPath = buildPath(destDir, newName);
        }
        
        // Perform move/rename
        bool success = fs->rename(srcPath, dstPath);
        
        statusMessage = success ? 
            "<div class='alert alert-success'>âœ… Moved to " + dstPath + "</div>" :
            "<div class='alert alert-error'>âŒ Move failed</div>";
        
        r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
    });
    
    // Upload handler
    static String uploadDir = "/";
    static String uploadStorage = "internal";
    server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *r) {
        r->redirect("/files?storage=" + uploadStorage + "&dir=" + uploadDir + "&edit=1");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File uploadFile;
        if (!index) {
            uploadDir = request->hasArg("path") ? request->arg("path") : "/";
            if (!uploadDir.startsWith("/")) uploadDir = "/" + uploadDir;
            uploadStorage = currentStorageView;
        }
        String fullPath = buildPath(uploadDir, filename);
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!index && targetFS) {
            Serial.printf("Upload to %s: %s\n", currentStorageView.c_str(), fullPath.c_str());
            uploadFile = targetFS->open(fullPath, "w");
        }
        
        if (uploadFile && len) {
            uploadFile.write(data, len);
        }
        
        if (final && uploadFile) {
            Serial.printf("Upload complete: %d bytes\n", index + len);
            uploadFile.close();
        }
    });
    
    // ========== MKDIR ==========
    server.on("/mkdir", HTTP_GET, [](AsyncWebServerRequest *r) {
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("name") || !targetFS) {
            r->redirect("/files");
            return;
        }
        String dir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        String name = sanitizeFilename(r->getParam("name")->value());
        String fullPath = buildPath(dir, name);
        
        if (targetFS->mkdir(fullPath)) {
            statusMessage = "<div class='alert alert-success'>âœ… Created: " + fullPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Failed to create folder</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + dir + "&edit=1");
    });
    
    // ========== RENAME ==========
    server.on("/rename", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path")) {
            r->redirect("/files");
            return;
        }
        String path = sanitizeFilename(r->getParam("path")->value());
        
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        sendChunkedHtml(r, "Rename", [path](Print& out) {
            out.print(F("<div class='header'><h1>âœï¸ Rename</h1><a href='/files' class='btn btn-secondary'>â† Cancel</a></div>"));
            out.print(F("<form action='/do_rename' method='GET'>"));
            out.printf("<input type='hidden' name='path' value='%s'>", path.c_str());
            out.printf("<input type='hidden' name='storage' value='%s'>", currentStorageView.c_str());
            
            // Extract filename
            int lastSlash = path.lastIndexOf('/');
            String filename = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
            
            out.print(F("<div class='section'>"));
            out.printf("<p>Renaming: <strong>%s</strong></p>", path.c_str());
            out.printf("<div class='form-group'><label>New Name</label><input type='text' name='newname' value='%s' required></div>", filename.c_str());
            out.print(F("</div>"));
            out.print(F("<button type='submit' class='btn'>ðŸ’¾ Rename</button></form>"));
        });
    });
    
    server.on("/do_rename", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("path") || !r->hasParam("newname") || !targetFS) {
            r->redirect("/files");
            return;
        }
        
        String oldPath = sanitizeFilename(r->getParam("path")->value());
        String newName = sanitizeFilename(r->getParam("newname")->value());
        
        int lastSlash = oldPath.lastIndexOf('/');
        String dir = lastSlash <= 0 ? "/" : oldPath.substring(0, lastSlash);
        String newPath = buildPath(dir, newName);
        
        if (targetFS->rename(oldPath, newPath)) {
            statusMessage = "<div class='alert alert-success'>âœ… Renamed to: " + newPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Rename failed</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + dir + "&edit=1");
    });
    
    // ========== MOVE ==========
    server.on("/move", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("path") || !r->hasParam("dest") || !targetFS) {
            r->redirect("/files");
            return;
        }
        
        String path = sanitizeFilename(r->getParam("path")->value());
        String dest = sanitizeFilename(r->getParam("dest")->value());
        
        int lastSlash = path.lastIndexOf('/');
        String filename = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
        String sourceDir = lastSlash <= 0 ? "/" : path.substring(0, lastSlash);
        
        String newPath = buildPath(dest, filename);
        
        if (targetFS->rename(path, newPath)) {
            statusMessage = "<div class='alert alert-success'>âœ… Moved to: " + newPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Move failed</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + sourceDir + "&edit=1");
    });
    
    // ========== CREATE NEW LOG ==========
    server.on("/create_log", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fsAvailable || !activeFS) {
            r->redirect("/settings_datalog");
            return;
        }
        
        // Generate filename with timestamp
        String folder = String(config.datalog.folder);
        if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
        if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
        if (folder.length() == 0) folder = "/";
        
        // Create folder if needed
        if (folder != "/" && !activeFS->exists(folder)) {
            activeFS->mkdir(folder);
        }
        
        // Generate new filename based on settings
        String newFile = folder + String(config.datalog.prefix);
        
        // Add device ID if enabled
        if (config.datalog.includeDeviceId && strlen(config.deviceId) > 0) {
            newFile += "_";
            newFile += config.deviceId;
        }
        
        // Add timestamp if enabled
        if (config.datalog.timestampFilename) {
            if (Rtc) {
                RtcDateTime now = Rtc->GetDateTime();
                char buf[20];
                snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
                    now.Year(), now.Month(), now.Day(),
                    now.Hour(), now.Minute(), now.Second());
                newFile += buf;
            } else {
                newFile += "_";
                newFile += String(millis());
            }
        }
        
        newFile += ".txt";
        
        // Create empty file
        File f = activeFS->open(newFile, "w");
        if (f) {
            f.close();
            strncpy(config.datalog.currentFile, newFile.c_str(), 64);
            saveConfig();
            statusMessage = "<div class='alert alert-success'>âœ… Created: " + newFile + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>âŒ Failed to create log</div>";
        }
        r->redirect("/settings_datalog");
    });
    
    // ========== EXPORT/IMPORT SETTINGS ==========
    server.on("/export_settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        
        // Device settings
        doc["deviceName"] = config.deviceName;
        doc["deviceId"] = config.deviceId;
        doc["forceWebServer"] = config.forceWebServer;
        
        // Theme settings
        JsonObject theme = doc["theme"].to<JsonObject>();
        theme["mode"] = config.theme.mode;
        theme["primaryColor"] = config.theme.primaryColor;
        theme["secondaryColor"] = config.theme.secondaryColor;
        theme["bgColor"] = config.theme.bgColor;
        theme["textColor"] = config.theme.textColor;
        theme["ffColor"] = config.theme.ffColor;
        theme["pfColor"] = config.theme.pfColor;
        theme["otherColor"] = config.theme.otherColor;
        theme["logoSource"] = config.theme.logoSource;
        theme["faviconPath"] = config.theme.faviconPath;
        theme["boardDiagramPath"] = config.theme.boardDiagramPath;
        theme["chartSource"] = config.theme.chartSource;
        theme["chartLocalPath"] = config.theme.chartLocalPath;
        
        // Flow meter settings
        JsonObject flowMeter = doc["flowMeter"].to<JsonObject>();
        flowMeter["pulsesPerLiter"] = config.flowMeter.pulsesPerLiter;
        flowMeter["calibrationMultiplier"] = config.flowMeter.calibrationMultiplier;
        flowMeter["monitoringWindowSecs"] = config.flowMeter.monitoringWindowSecs;
        flowMeter["firstLoopMonitoringWindowSecs"] = config.flowMeter.firstLoopMonitoringWindowSecs;
        flowMeter["blinkDuration"] = config.flowMeter.blinkDuration;
        
        // Datalog settings
        JsonObject datalog = doc["datalog"].to<JsonObject>();
        datalog["rotation"] = config.datalog.rotation;
        datalog["maxSizeKB"] = config.datalog.maxSizeKB;
        datalog["maxEntries"] = config.datalog.maxEntries;
        datalog["folder"] = config.datalog.folder;
        datalog["prefix"] = config.datalog.prefix;
        datalog["dateFormat"] = config.datalog.dateFormat;
        datalog["timeFormat"] = config.datalog.timeFormat;
        datalog["endFormat"] = config.datalog.endFormat;
        datalog["volumeFormat"] = config.datalog.volumeFormat;
        datalog["includeBootCount"] = config.datalog.includeBootCount;
        datalog["includeExtraPresses"] = config.datalog.includeExtraPresses;
        datalog["postCorrectionEnabled"] = config.datalog.postCorrectionEnabled;
        datalog["pfToFfThreshold"] = config.datalog.pfToFfThreshold;
        datalog["ffToPfThreshold"] = config.datalog.ffToPfThreshold;
        datalog["manualPressThresholdMs"] = config.datalog.manualPressThresholdMs;
        
        // Network settings
        JsonObject network = doc["network"].to<JsonObject>();
        network["wifiMode"] = config.network.wifiMode;
        network["apSSID"] = config.network.apSSID;
        network["clientSSID"] = config.network.clientSSID;
        network["ntpServer"] = config.network.ntpServer;
        network["timezone"] = config.network.timezone;
        network["useStaticIP"] = config.network.useStaticIP;
        
        // Hardware settings
        JsonObject hardware = doc["hardware"].to<JsonObject>();
        hardware["storageType"] = config.hardware.storageType;
        hardware["wakeupMode"] = config.hardware.wakeupMode;
        hardware["cpuFreqMHz"] = config.hardware.cpuFreqMHz;
        hardware["debugMode"] = config.hardware.debugMode;
        hardware["defaultStorageView"] = config.hardware.defaultStorageView;
        hardware["debounceMs"] = config.hardware.debounceMs;
        
        String json;
        serializeJsonPretty(doc, json);
        
        AsyncWebServerResponse *response = r->beginResponse(200, "application/json", json);
        String filename = String(config.deviceName) + "_settings.json";
        response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        r->send(response);
    });
    
    // Import settings from JSON file
    static String importBuffer;
    server.on("/import_settings", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Process the buffered JSON
        if (importBuffer.length() == 0) {
            r->send(400, "text/plain", "No data received");
            return;
        }
        
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, importBuffer);
        importBuffer = "";  // Clear buffer
        
        if (err) {
            r->send(400, "text/plain", String("JSON parse error: ") + err.c_str());
            return;
        }
        
        // Import device settings
        if (doc.containsKey("deviceName")) {
            strncpy(config.deviceName, doc["deviceName"] | config.deviceName, 32);
        }
        if (doc.containsKey("forceWebServer")) {
            config.forceWebServer = doc["forceWebServer"] | config.forceWebServer;
        }
        
        // Import theme settings
        if (doc.containsKey("theme")) {
            JsonObject theme = doc["theme"];
            config.theme.mode = theme["mode"] | config.theme.mode;
            strncpy(config.theme.primaryColor, theme["primaryColor"] | config.theme.primaryColor, 8);
            strncpy(config.theme.secondaryColor, theme["secondaryColor"] | config.theme.secondaryColor, 8);
            strncpy(config.theme.bgColor, theme["bgColor"] | config.theme.bgColor, 8);
            strncpy(config.theme.textColor, theme["textColor"] | config.theme.textColor, 8);
            strncpy(config.theme.ffColor, theme["ffColor"] | config.theme.ffColor, 8);
            strncpy(config.theme.pfColor, theme["pfColor"] | config.theme.pfColor, 8);
            strncpy(config.theme.otherColor, theme["otherColor"] | config.theme.otherColor, 8);
            strncpy(config.theme.logoSource, theme["logoSource"] | config.theme.logoSource, 128);
            strncpy(config.theme.faviconPath, theme["faviconPath"] | config.theme.faviconPath, 32);
            strncpy(config.theme.boardDiagramPath, theme["boardDiagramPath"] | config.theme.boardDiagramPath, 64);
            config.theme.chartSource = theme["chartSource"] | config.theme.chartSource;
            strncpy(config.theme.chartLocalPath, theme["chartLocalPath"] | config.theme.chartLocalPath, 64);
        }
        
        // Import flow meter settings
        if (doc.containsKey("flowMeter")) {
            JsonObject fm = doc["flowMeter"];
            config.flowMeter.pulsesPerLiter = fm["pulsesPerLiter"] | config.flowMeter.pulsesPerLiter;
            config.flowMeter.calibrationMultiplier = fm["calibrationMultiplier"] | config.flowMeter.calibrationMultiplier;
            config.flowMeter.monitoringWindowSecs = fm["monitoringWindowSecs"] | config.flowMeter.monitoringWindowSecs;
            config.flowMeter.firstLoopMonitoringWindowSecs = fm["firstLoopMonitoringWindowSecs"] | config.flowMeter.firstLoopMonitoringWindowSecs;
            config.flowMeter.blinkDuration = fm["blinkDuration"] | config.flowMeter.blinkDuration;
        }
        
        // Import datalog settings
        if (doc.containsKey("datalog")) {
            JsonObject dl = doc["datalog"];
            config.datalog.rotation = dl["rotation"] | config.datalog.rotation;
            config.datalog.maxSizeKB = dl["maxSizeKB"] | config.datalog.maxSizeKB;
            config.datalog.maxEntries = dl["maxEntries"] | config.datalog.maxEntries;
            strncpy(config.datalog.folder, dl["folder"] | config.datalog.folder, 32);
            strncpy(config.datalog.prefix, dl["prefix"] | config.datalog.prefix, 32);
            config.datalog.dateFormat = dl["dateFormat"] | config.datalog.dateFormat;
            config.datalog.timeFormat = dl["timeFormat"] | config.datalog.timeFormat;
            config.datalog.endFormat = dl["endFormat"] | config.datalog.endFormat;
            config.datalog.volumeFormat = dl["volumeFormat"] | config.datalog.volumeFormat;
            config.datalog.includeBootCount = dl["includeBootCount"] | config.datalog.includeBootCount;
            config.datalog.includeExtraPresses = dl["includeExtraPresses"] | config.datalog.includeExtraPresses;
            if (dl.containsKey("postCorrectionEnabled"))
                config.datalog.postCorrectionEnabled = dl["postCorrectionEnabled"] | config.datalog.postCorrectionEnabled;
            if (dl.containsKey("pfToFfThreshold"))
                config.datalog.pfToFfThreshold = dl["pfToFfThreshold"] | config.datalog.pfToFfThreshold;
            if (dl.containsKey("ffToPfThreshold"))
                config.datalog.ffToPfThreshold = dl["ffToPfThreshold"] | config.datalog.ffToPfThreshold;
            if (dl.containsKey("manualPressThresholdMs"))
                config.datalog.manualPressThresholdMs = dl["manualPressThresholdMs"] | config.datalog.manualPressThresholdMs;
        }
        
        // Import network settings
        if (doc.containsKey("network")) {
            JsonObject net = doc["network"];
            config.network.wifiMode = net["wifiMode"] | config.network.wifiMode;
            strncpy(config.network.apSSID, net["apSSID"] | config.network.apSSID, 32);
            strncpy(config.network.clientSSID, net["clientSSID"] | config.network.clientSSID, 32);
            strncpy(config.network.ntpServer, net["ntpServer"] | config.network.ntpServer, 64);
            config.network.timezone = net["timezone"] | config.network.timezone;
            config.network.useStaticIP = net["useStaticIP"] | config.network.useStaticIP;
        }
        
        // Import hardware settings
        if (doc.containsKey("hardware")) {
            JsonObject hw = doc["hardware"];
            config.hardware.storageType = hw["storageType"] | config.hardware.storageType;
            config.hardware.wakeupMode = hw["wakeupMode"] | config.hardware.wakeupMode;
            config.hardware.cpuFreqMHz = hw["cpuFreqMHz"] | config.hardware.cpuFreqMHz;
            config.hardware.debugMode = hw["debugMode"] | config.hardware.debugMode;
            config.hardware.defaultStorageView = hw["defaultStorageView"] | config.hardware.defaultStorageView;
            config.hardware.debounceMs = hw["debounceMs"] | config.hardware.debounceMs;
        }
        
        saveConfig();
        r->send(200, "text/plain", "OK");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        // File upload handler - accumulate data
        if (!index) {
            importBuffer = "";
        }
        for (size_t i = 0; i < len; i++) {
            importBuffer += (char)data[i];
        }
    });
    
    // ========== WIFI SCAN ==========
    server.on("/wifi_scan_start", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Block WiFi scan during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            r->send(503, "text/plain", "Busy - measurement in progress");
            return;
        }
        // Must be in AP+STA mode to scan while serving AP
        WiFi.mode(WIFI_AP_STA);
        WiFi.scanDelete();
        int result = WiFi.scanNetworks(true);  // async=true
        DBGF("WiFi scan started, result: %d\n", result);
        r->send(200, "text/plain", "OK");
    });
    
    server.on("/wifi_scan_result", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<2048> doc;
        JsonArray networks = doc.createNestedArray("networks");
        
        int n = WiFi.scanComplete();
        DBGF("WiFi scan complete: %d networks\n", n);
        
        if (n == WIFI_SCAN_RUNNING) {
            doc["scanning"] = true;
        } else if (n == WIFI_SCAN_FAILED) {
            doc["error"] = "Scan failed";
        } else if (n >= 0) {
            for (int i = 0; i < n && i < 20; i++) {
                JsonObject net = networks.createNestedObject();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
        }
        
        sendJsonResponse(r, doc);
    });
    
    server.on("/wifi_test", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<256> doc;
        
        // Block WiFi test during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            doc["success"] = false;
            doc["error"] = "Cannot test WiFi during active measurement";
            sendJsonResponse(r, doc);
            return;
        }
        
        if (!r->hasParam("ssid")) {
            doc["success"] = false;
            doc["error"] = "No SSID";
            sendJsonResponse(r, doc);
            return;
        }
        
        String ssid = r->getParam("ssid")->value();
        String pass = r->hasParam("pass") ? r->getParam("pass")->value() : "";
        
        WiFi.disconnect();
        yield();  // Non-blocking
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        
        // Non-blocking wait with yield
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
            yield();  // Allow background tasks
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            doc["success"] = true;
            doc["ip"] = WiFi.localIP().toString();
            WiFi.disconnect();
        } else {
            doc["success"] = false;
            doc["error"] = "Connection failed";
        }
        
        // Restore original mode
        if (config.network.wifiMode == WIFIMODE_AP) {
            startAPMode();
        }
        
        sendJsonResponse(r, doc);
    });
    
    // ========== STATIC FILE HANDLER ==========
    // System files (logo, board, favicon, chart, changelog) - always from LittleFS
    // Other files - from activeFS (SD or LittleFS based on config)
    server.onNotFound([](AsyncWebServerRequest *r) {
        String path = r->url();
        
        // Determine content type
        String contentType = "application/octet-stream";
        if (path.endsWith(".svg")) contentType = "image/svg+xml";
        else if (path.endsWith(".png")) contentType = "image/png";
        else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) contentType = "image/jpeg";
        else if (path.endsWith(".gif")) contentType = "image/gif";
        else if (path.endsWith(".ico")) contentType = "image/x-icon";
        else if (path.endsWith(".css")) contentType = "text/css";
        else if (path.endsWith(".js")) contentType = "application/javascript";
        else if (path.endsWith(".json")) contentType = "application/json";
        else if (path.endsWith(".html") || path.endsWith(".htm")) contentType = "text/html";
        else if (path.endsWith(".txt") || path.endsWith(".log") || path.endsWith(".csv")) contentType = "text/plain";
        
        // System files - ALWAYS check LittleFS first
        bool isSystemFile = path.startsWith("/logo") || path.startsWith("/board") || 
                           path.startsWith("/favicon") || path.startsWith("/chart") ||
                           path == "/changelog.txt" || path == "/style.css";
        
        if (isSystemFile && littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, contentType);
            return;
        }
        
        // Try activeFS (SD or LittleFS based on config)
        if (fsAvailable && activeFS && activeFS->exists(path)) {
            r->send(*activeFS, path, contentType);
            return;
        }
        
        // Fallback to LittleFS for any file
        if (littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, contentType);
            return;
        }
        
        r->send(404, "text/plain", "Not found");
    });
    
    // ========== CUSTOM OTA UPDATE ==========
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Update", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("ðŸ“¤"));
            out.printf("%s</h1>", "Update");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Update");
            
            // Popup overlay for status messages
            out.print(F("<div id='popup' class='popup-overlay' style='display:none'>"));
            out.print(F("<div class='popup-content'>"));
            out.print(F("<div id='popupIcon' style='font-size:4rem;margin-bottom:1rem'></div>"));
            out.print(F("<div id='popupTitle' style='font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem'></div>"));
            out.print(F("<div id='popupMsg' style='margin-bottom:1rem'></div>"));
            out.print(F("<div id='popupProgress' style='display:none'>"));
            out.print(F("<div style='background:#e0e0e0;border-radius:4px;height:12px;margin-bottom:0.5rem;overflow:hidden'><div id='popupBar' style='width:0%;height:100%;background:#3498db;transition:width 0.3s'></div></div>"));
            out.print(F("<div id='popupCounter'></div>"));
            out.print(F("</div>"));
            out.print(F("<button id='popupClose' class='btn btn-secondary' onclick='closePopup()' style='display:none'>Close</button>"));
            out.print(F("</div></div>"));
            
            // Current firmware card
            out.printf("<div class='card'><div class='card-header'>ðŸ“‹ %s</div><div class='card-body'>", "Firmware");
            out.print(F("<div class='form-row'>"));
            out.printf("<div><strong>%s</strong><div class='text-primary'>", "Version");
            out.print(getVersionString());
            out.print(F("</div></div>"));
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Chip", ESP.getChipModel());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Flash", formatFileSize(ESP.getFlashChipSize()).c_str());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Free", formatFileSize(ESP.getFreeSketchSpace()).c_str());
            out.print(F("</div></div></div>"));
            
            // Upload card
            out.printf("<div class='card'><div class='card-header'>ðŸ“¤ %s</div><div class='card-body'>", "Upload");
            out.print(F("<form id='uploadForm' enctype='multipart/form-data'>"));
            out.print(F("<div class='form-group'>"));
            out.printf("<label class='form-label'>%s (.bin)</label>", "Select file");
            out.print(F("<input type='file' id='firmware' name='firmware' accept='.bin' required class='form-input'>"));
            out.print(F("</div>"));
            out.print(F("<div id='fileInfo' class='alert' style='display:none'></div>"));
            out.printf("<button type='submit' id='uploadBtn' class='btn btn-primary btn-block' disabled>ðŸ“¤ %s</button>", "Upload");
            out.print(F("</form>"));
            
            // Progress inside form
            out.print(F("<div id='progress' style='display:none;margin-top:1rem'>"));
            out.print(F("<div class='progress' style='height:24px'>"));
            out.print(F("<div id='progressBar' class='progress-bar progress-bar-success' style='width:0%'></div>"));
            out.print(F("</div>"));
            out.print(F("<p id='progressText' class='text-center mt-1'>0%</p>"));
            out.print(F("</div>"));
            out.print(F("</div></div>"));
            
            // Safety notes card
            out.printf("<div class='card'><div class='card-header'>âš ï¸ %s</div><div class='card-body'>", "Safety Notes");
            out.print(F("<ul style='margin:0;padding-left:1.2rem;font-size:0.9rem'>"));
            out.print(F("<li>.bin files only</li>"));
            out.printf("<li>%s</li>", "Do not disconnect power");
            out.printf("<li>%s</li>", "Restart");
            out.print(F("</ul></div></div>"));
            
            // JavaScript
            out.print(F("<script>"));
            out.printf("var maxSize=%u;", ESP.getFreeSketchSpace());
            out.print(F(R"(
                var fileInput=document.getElementById('firmware');
                var uploadBtn=document.getElementById('uploadBtn');
                var fileInfo=document.getElementById('fileInfo');
                var progressDiv=document.getElementById('progress');
                var progressBar=document.getElementById('progressBar');
                var progressText=document.getElementById('progressText');
                
                // Popup functions
                function showPopup(icon,title,msg,showProgress,showClose){
                    document.getElementById('popupIcon').textContent=icon;
                    document.getElementById('popupTitle').textContent=title;
                    document.getElementById('popupMsg').innerHTML=msg;
                    document.getElementById('popupProgress').style.display=showProgress?'block':'none';
                    document.getElementById('popupClose').style.display=showClose?'inline-block':'none';
                    document.getElementById('popup').style.display='flex';
                }
                function closePopup(){document.getElementById('popup').style.display='none';}
                function updatePopupProgress(pct,text){
                    document.getElementById('popupBar').style.width=pct+'%';
                    document.getElementById('popupCounter').textContent=text;
                }

                fileInput.onchange=function(){
                    var file=this.files[0];
                    if(!file){uploadBtn.disabled=true;fileInfo.style.display='none';return;}
                    
                    var errors=[];
                    if(!file.name.endsWith('.bin'))errors.push('File must be a .bin file');
                    if(file.size<10000)errors.push('File too small (min 10KB)');
                    // Note: maxSize is ESP.getFreeSketchSpace(), actual limit may differ
                    
                    if(errors.length>0){
                        fileInfo.innerHTML='<span style="color:#c00">âŒ '+errors.join('<br>')+'</span>';
                        fileInfo.style.display='block';
                        uploadBtn.disabled=true;
                        return;
                    }
                    
                    var reader=new FileReader();
                    reader.onload=function(e){
                        var arr=new Uint8Array(e.target.result);
                        if(arr[0]!==0xE9){
                            fileInfo.innerHTML='<span style="color:#c00">âŒ Invalid firmware file</span>';
                            fileInfo.style.display='block';
                            uploadBtn.disabled=true;
                            return;
                        }
                        fileInfo.innerHTML='<span style="color:#080">âœ… '+file.name+' ('+Math.round(file.size/1024)+'KB)</span>';
                        fileInfo.style.display='block';
                        uploadBtn.disabled=false;
                    };
                    reader.readAsArrayBuffer(file.slice(0,4));
                };

                document.getElementById('uploadForm').onsubmit=function(e){
                    e.preventDefault();
                    var file=fileInput.files[0];
                    if(!file)return;
                    
                    uploadBtn.disabled=true;
                    fileInput.disabled=true;
                    showPopup('ðŸ“¤','Uploading...','Please wait while firmware is being uploaded.',true,false);
                    
                    var xhr=new XMLHttpRequest();
                    xhr.upload.onprogress=function(e){
                        if(e.lengthComputable){
                            var pct=Math.round(e.loaded/e.total*100);
                            progressBar.style.width=pct+'%';
                            progressText.textContent=pct+'%';
                            updatePopupProgress(pct,Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB');
                        }
                    };
                    xhr.onload=function(){
                        progressDiv.style.display='none';
                        if(xhr.status===200){
                            var resp=JSON.parse(xhr.responseText);
                            if(resp.success){
                                var seconds=5;
                                var tick=function(){
                                    showPopup('âœ…','Update Complete!','Device will restart...<br>Redirecting in <strong>'+seconds+'</strong> seconds',true,false);
                                    updatePopupProgress((5-seconds)*20,'');
                                    if(seconds<=0){window.location.href='/';}
                                    else{seconds--;setTimeout(tick,1000);}
                                };
                                tick();
                            }else{
                                showPopup('âŒ','Update Failed',resp.message,false,true);
                                uploadBtn.disabled=false;
                                fileInput.disabled=false;
                            }
                        }else{
                            showPopup('âŒ','Upload Error','Server returned: '+xhr.statusText,false,true);
                            uploadBtn.disabled=false;
                            fileInput.disabled=false;
                        }
                    };
                    xhr.onerror=function(){
                        showPopup('âŒ','Connection Error','Could not connect to device',false,true);
                        uploadBtn.disabled=false;
                        fileInput.disabled=false;
                    };
                    
                    var formData=new FormData();
                    formData.append('firmware',file);
                    xhr.open('POST','/do_update');
                    xhr.send(formData);
                };
                )"));
            out.print(F("</script>"));
        });
    });
    
    // OTA upload handler
    // OTA update error tracking
    static bool otaStarted = false;
    static bool otaSuccess = false;
    static String otaError = "";
    
    server.on("/do_update", HTTP_POST, 
        // Response handler (called after upload completes)
        [](AsyncWebServerRequest *r) {
            bool success = otaStarted && otaSuccess && !Update.hasError();
            StaticJsonDocument<128> doc;
            doc["success"] = success;
            if (success) {
                doc["message"] = "Firmware updated successfully!";
            } else if (otaError.length() > 0) {
                doc["message"] = otaError;
            } else if (Update.hasError()) {
                doc["message"] = Update.errorString();
            } else {
                doc["message"] = "Update failed - no data written";
            }
            String response;
            serializeJson(doc, response);
            
            // Reset for next attempt
            otaStarted = false;
            otaSuccess = false;
            otaError = "";
            
            AsyncWebServerResponse *resp = r->beginResponse(200, "application/json", response);
            resp->addHeader("Connection", "close");
            r->send(resp);
            
            if (success) {
                shouldRestart = true;
                restartTimer = millis();
            }
        },
        // Upload handler (called for each chunk)
        [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                // Reset state for new upload
                otaStarted = false;
                otaSuccess = false;
                otaError = "";
                
                // First chunk - start update
                Serial.printf("OTA Update: %s (%u bytes)\n", filename.c_str(), r->contentLength());
                
                // Validate filename
                if (!filename.endsWith(".bin")) {
                    otaError = "Not a .bin file";
                    Serial.println("OTA Error: " + otaError);
                    return;
                }
                
                // Validate first byte (ESP32 magic)
                if (len > 0 && data[0] != 0xE9) {
                    otaError = "Invalid firmware file (wrong magic byte)";
                    Serial.println("OTA Error: " + otaError);
                    return;
                }
                
                // Validate size
                if (r->contentLength() < 10000) {
                    otaError = "File too small";
                    Serial.println("OTA Error: " + otaError);
                    return;
                }
                
                // Start update
                if (!Update.begin(r->contentLength(), U_FLASH)) {
                    otaError = Update.errorString();
                    Serial.printf("OTA Error: %s\n", otaError.c_str());
                    return;
                }
                otaStarted = true;
                Serial.println("OTA Update started...");
            }
            
            // Write chunk only if update was started successfully
            if (otaStarted && Update.isRunning()) {
                if (Update.write(data, len) != len) {
                    otaError = Update.errorString();
                    Serial.printf("OTA Write Error: %s\n", otaError.c_str());
                }
            }
            
            // Final chunk
            if (final && otaStarted) {
                if (Update.end(true)) {
                    otaSuccess = true;
                    Serial.printf("OTA Update complete: %u bytes\n", index + len);
                } else {
                    otaError = Update.errorString();
                    Serial.printf("OTA End Error: %s\n", otaError.c_str());
                }
            }
        }
    );
    
    server.begin();
    DBGF("Web server started. Free heap: %d\n", ESP.getFreeHeap());
}

// ============================================================================
// RECURSIVE DELETE
// ============================================================================
bool deleteRecursive(fs::FS &fs, const String &path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        return fs.remove(path);
    }
    
    while (File entry = dir.openNextFile()) {
        String childPath = path + "/" + String(entry.name());
        bool isDir = entry.isDirectory();
        entry.close();
        
        if (isDir) {
            deleteRecursive(fs, childPath);
        } else {
            fs.remove(childPath);
        }
    }
    dir.close();
    return fs.rmdir(path);
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    // === EARLY GPIO SNAPSHOT ===
    // Capture ALL GPIO pin states IMMEDIATELY at boot, before any delays.
    // Reed switches are momentary - the magnet may pass within 50-200ms.
    // By the time loadConfig/initStorage/initHardware complete (~300-500ms),
    // the triggering switch is likely already open.
    // Stores raw values in a bitmask; interpreted later using actual config pin numbers.
    // Pin mode defaults after deep sleep retain the pullup/pulldown configured before sleep.
    {
        earlyGPIO_bitmask = 0;
        for (uint8_t pin = 0; pin <= 10; pin++) {
            if (digitalRead(pin)) {
                earlyGPIO_bitmask |= (1UL << pin);
            }
        }
        // Also read pins 20-21 (some ESP32-C3 boards use these)
        if (digitalRead(20)) earlyGPIO_bitmask |= (1UL << 20);
        if (digitalRead(21)) earlyGPIO_bitmask |= (1UL << 21);
        earlyGPIO_captured = true;
        earlyGPIO_millis = millis();
    }
    
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== ESP32 Water Logger v4.1.4 ===");
    Serial.printf("Early GPIO bitmask: 0x%08X\n", earlyGPIO_bitmask);
    
    loadConfig();
    
    isrDebounceUs = (unsigned long)config.hardware.debounceMs * 1000UL;  // Convert ms to microseconds for ISR
    initStorage();
    initHardware();
    
    // Measure flush button hold duration.
    // Reed switch stays closed while flush plate button is held down.
    // Normal press: ~200-400ms. Extended hold: user deliberately holds button
    // longer, releasing more water than usual. If held >= manualPressThresholdMs,
    // post-correction is skipped (extra volume is intentional, not misidentification).
    if (earlyGPIO_captured) {
        int expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
        bool ffStillActive = (digitalRead(config.hardware.pinWakeupFF) == expectedActive);
        bool pfStillActive = (digitalRead(config.hardware.pinWakeupPF) == expectedActive);
        
        if (ffStillActive || pfStillActive) {
            // Button still held - wait until released or timeout
            unsigned long holdStart = earlyGPIO_millis;
            unsigned long maxWait = 5000;
            while ((millis() - holdStart) < maxWait) {
                ffStillActive = (digitalRead(config.hardware.pinWakeupFF) == expectedActive);
                pfStillActive = (digitalRead(config.hardware.pinWakeupPF) == expectedActive);
                if (!ffStillActive && !pfStillActive) break;
                delay(10);
            }
            buttonHeldMs = millis() - holdStart;
        } else {
            // Already released - quick button press
            buttonHeldMs = millis() - earlyGPIO_millis;
        }
        Serial.printf("Button held: %lums\n", buttonHeldMs);
    }
    
    // Capture wake timestamp from RTC
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
    }
    
    if (bootcount_restore) {
        restoreBootCount();
        bootcount_restore = false;
    }
    
    bootCount++;
    backupBootCount();
    DBGF("Boot count: %d\n", bootCount);
    
    // Check wake-up reason and WiFi trigger
    wakeUpButtonStr = getWakeupReason();
    Serial.printf("Wake reason: %s\n", wakeUpButtonStr.c_str());
    
    // Determine if we should enter web server mode
    int wifiTriggerState = digitalRead(config.hardware.pinWifiTrigger);
    int expectedActive = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
    
    apModeTriggered = (wifiTriggerState == expectedActive) || 
                      (wakeUpButtonStr == "WIFI") || 
                      config.forceWebServer;
    
    // Online Logger Mode = forceWebServer enabled but NOT triggered by WiFi button
    onlineLoggerMode = config.forceWebServer && 
                       (wifiTriggerState != expectedActive) && 
                       (wakeUpButtonStr != "WIFI");
    
    if (apModeTriggered) {
        // Web Server Mode (or Online Logger Mode)
        Serial.println(onlineLoggerMode ? "=== Online Logger Mode ===" : "=== Web Server Mode ===");
        setCpuFrequencyMhz(160);
        
        if (!onlineLoggerMode) {
            flushLogBufferToFS();  // Flush any pending logs (only if pure web server mode)
        }
        
        if (config.network.wifiMode == WIFIMODE_CLIENT) {
            Serial.println("Trying client mode...");
            if (!connectToWiFi()) {
                DBGLN("Client failed, fallback to AP");
                wifiFallbackToAP = true;
                startAPMode();
            }
        } else {
            startAPMode();
        }
        setupWebServer();
        
        // If Online Logger Mode, also attach flow sensor interrupt
        if (onlineLoggerMode) {
            DBGLN("Attaching flow sensor for Online Logger Mode...");
            attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor), onFlowPulse, FALLING);
            configureWakeup();  // Setup wakeup pins (but won't sleep)
        }
    } else {
        // Normal Logging Mode (will enter deep sleep after cycle)
        DBGLN("=== Normal Logging Mode ===");
        setCpuFrequencyMhz(config.hardware.cpuFreqMHz);
        // Note: configureWakeup() is called right before esp_deep_sleep_start()
        // to avoid any interference during active operation
        
        // Attach flow sensor interrupt only in logging mode
        attachInterrupt(digitalPinToInterrupt(config.hardware.pinFlowSensor), onFlowPulse, FALLING);
    }
    
    // Initialize logging cycle
    lastLoggingCycleStartTime = millis();
    cycleStartTime = millis();
    stateStartTime = millis();
    lastFlowPulseTime = 0;
    cycleStartedBy = wakeUpButtonStr.length() > 0 ? wakeUpButtonStr : "BOOT";
    
    // Initialize button states with current readings to prevent false counts
    int currentFFState = digitalRead(config.hardware.pinWakeupFF);
    int currentPFState = digitalRead(config.hardware.pinWakeupPF);
    // expectedActive already declared above
    
    stableFFState = currentFFState;
    stablePFState = currentPFState;
    lastFFButtonState = currentFFState;
    lastPFButtonState = currentPFState;
    lastFFDebounceTime = millis();
    lastPFDebounceTime = millis();
    
    // Initialize state machine based on wake-up reason
    // Note: The triggering button is recorded in cycleStartedBy, but NOT counted in highCountFF/PF
    // highCountFF/PF are only for ADDITIONAL presses during the monitoring cycle
    if (wakeUpButtonStr == "FF_BTN") {
        // Woken from deep sleep by FF button - start flow wait
        cycleButtonSet = true;
        cycleStartedBy = "FF_BTN";
        loggingState = STATE_WAIT_FLOW;
        // highCountFF stays 0 - the trigger is recorded in cycleStartedBy, not as extra press
        Serial.println("Wake by FF_BTN -> STATE_WAIT_FLOW");
    } else if (wakeUpButtonStr == "PF_BTN") {
        // Woken from deep sleep by PF button - start flow wait
        cycleButtonSet = true;
        cycleStartedBy = "PF_BTN";
        loggingState = STATE_WAIT_FLOW;
        // highCountPF stays 0 - the trigger is recorded in cycleStartedBy, not as extra press
        Serial.println("Wake by PF_BTN -> STATE_WAIT_FLOW");
    } else if (onlineLoggerMode) {
        // Online Logger Mode - check if button is pressed at startup
        if (currentFFState == expectedActive) {
            cycleStartedBy = "FF_BTN";
            cycleButtonSet = true;
            loggingState = STATE_WAIT_FLOW;
            // highCountFF stays 0
            DBGLN("Online Logger: FF button at startup -> STATE_WAIT_FLOW");
        } else if (currentPFState == expectedActive) {
            cycleStartedBy = "PF_BTN";
            cycleButtonSet = true;
            loggingState = STATE_WAIT_FLOW;
            // highCountPF stays 0
            DBGLN("Online Logger: PF button at startup -> STATE_WAIT_FLOW");
        } else {
            loggingState = STATE_IDLE;
            DBGLN("Online Logger: No button -> STATE_IDLE");
        }
    } else {
        // Normal mode without button wakeup - wait for button
        loggingState = STATE_IDLE;
        DBGLN("Normal mode -> STATE_IDLE (waiting for button)" );
    }
    
    Serial.printf("Button states init: FF=%d PF=%d, WakeReason=%s, CycleBy=%s, State=%d\n", 
        currentFFState, currentPFState, wakeUpButtonStr.c_str(), cycleStartedBy.c_str(), loggingState);
    Serial.println("Setup complete!");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
    // Check for restart request FIRST (works in all modes)
    if (shouldRestart && millis() - restartTimer > 2000) {
        Serial.println("Restarting...");
        Serial.flush();
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    }
    
    // Pure Web Server mode (WiFi button triggered) - just wait
    if (apModeTriggered && !onlineLoggerMode) {
        delay(10);  // Prevent tight loop, AsyncWebServer handles requests
        return;
    }
    
    // === LOGGING CODE (runs in Normal mode OR Online Logger mode) ===
    
    // Process buttons via polling (more reliable than ISR for buttons)
    debounceButton(config.hardware.pinWakeupFF, lastFFButtonState, stableFFState, lastFFDebounceTime, highCountFF);
    debounceButton(config.hardware.pinWakeupPF, lastPFButtonState, stablePFState, lastPFDebounceTime, highCountPF);
    
    // Track flow pulse timing
    if (flowSensorPulseDetected) {
        flowSensorPulseDetected = false;
        lastFlowPulseTime = millis();
    }
    
    // Test mode - blink LED on flow pulses
    if (config.flowMeter.testMode) {
        static bool pinConfigured = false;
        if (!pinConfigured) { 
            pinMode(config.hardware.pinWifiTrigger, OUTPUT); 
            pinConfigured = true; 
        }
        
        if (pulseCount > 0 && lastFlowPulseTime > 0) {
            if (millis() - lastFlowPulseTime < 100) {
                // Blink while receiving pulses
                digitalWrite(config.hardware.pinWifiTrigger, (millis() / config.flowMeter.blinkDuration) % 2);
            } else if (millis() - lastFlowPulseTime < TEST_MODE_HOLD_MS) {
                digitalWrite(config.hardware.pinWifiTrigger, HIGH);
            } else {
                digitalWrite(config.hardware.pinWifiTrigger, LOW);
            }
        } else {
            digitalWrite(config.hardware.pinWifiTrigger, LOW);
        }
    }
    
    // ========== STATE MACHINE ==========
    switch (loggingState) {
        
        case STATE_IDLE:
            // Waiting for button press (FF or PF)
            // The triggering button starts the cycle but is NOT counted as extra press
            if (highCountFF > 0) {
                cycleStartedBy = "FF_BTN";
                cycleButtonSet = true;
                loggingState = STATE_WAIT_FLOW;
                stateStartTime = millis();
                cycleStartTime = millis();
                // Update wake timestamp for Online Logger mode
                if (onlineLoggerMode && Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
                }
                highCountFF = 0;  // Reset - trigger is in cycleStartedBy, not extra press
                highCountPF = 0;
                DBGLN("STATE: Button FF pressed -> WAIT_FLOW");
            } else if (highCountPF > 0) {
                cycleStartedBy = "PF_BTN";
                cycleButtonSet = true;
                loggingState = STATE_WAIT_FLOW;
                stateStartTime = millis();
                cycleStartTime = millis();
                // Update wake timestamp for Online Logger mode
                if (onlineLoggerMode && Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    currentWakeTimestamp = now.IsValid() ? now.Unix32Time() : 0;
                }
                highCountFF = 0;
                highCountPF = 0;  // Reset - trigger is in cycleStartedBy, not extra press
                DBGLN("STATE: Button PF pressed -> WAIT_FLOW");
            } else if (!onlineLoggerMode && !apModeTriggered && millis() - stateStartTime >= 2000) {
                // Normal mode: No button pressed for 2 seconds after boot -> go to sleep
                // This handles first boot or power cycle without button press
                DBGLN("STATE: No button in Normal mode -> going to sleep");
                configureWakeup();
                Serial.flush();
                esp_deep_sleep_start();
            }
            break;
            
        case STATE_WAIT_FLOW:
            // Button was pressed, waiting for flow (6 sec window)
            if (pulseCount > 0) {
                // Flow detected! Start monitoring
                loggingState = STATE_MONITORING;
                stateStartTime = millis();
                DBGF("STATE: Flow detected (%ld pulses) -> MONITORING\n", pulseCount);
            } else if (millis() - stateStartTime >= BUTTON_WAIT_FLOW_MS) {
                // Timeout - no flow detected within 6 seconds
                loggingState = STATE_DONE;
                DBGLN("STATE: No flow timeout -> DONE (logging 0 flow)");
            }
            break;
            
        case STATE_MONITORING:
            // Flow started, wait 3 seconds after last pulse
            if (millis() - lastFlowPulseTime >= FLOW_IDLE_TIMEOUT_MS) {
                // 3 seconds since last pulse - done monitoring
                loggingState = STATE_DONE;
                DBGF("STATE: Flow idle timeout -> DONE (%ld pulses total)\n", pulseCount);
            }
            break;
            
        case STATE_DONE:
            // Log and sleep/reset
            {
                // Atomic read of pulseCount for consistent decision
                noInterrupts();
                uint32_t currentPulses = pulseCount;
                interrupts();
                
                bool hasActivity = (currentPulses > 0 || highCountFF > 0 || highCountPF > 0);
                
                if (hasActivity) {
                    // === Volume-based post-correction ===
                    // Corrects button identification based on measured water volume.
                    // Conditions: enabled, clean cycle (no extra presses), and button
                    // was not held longer than threshold (extended hold = intentional,
                    // extra volume is expected, not a misidentification).
                    float correctionVolume = (float)currentPulses / config.flowMeter.pulsesPerLiter * config.flowMeter.calibrationMultiplier;
                    bool extendedHold = (config.datalog.manualPressThresholdMs > 0) && (buttonHeldMs >= config.datalog.manualPressThresholdMs);
                    if (config.datalog.postCorrectionEnabled && highCountFF == 0 && highCountPF == 0 && correctionVolume > 0 && !extendedHold) {
                        String origButton = cycleStartedBy;
                        bool corrected = false;
                        
                        if (cycleStartedBy == "PF_BTN" && correctionVolume >= config.datalog.pfToFfThreshold) {
                            cycleStartedBy = "FF_BTN";
                            if (!onlineLoggerMode) wakeUpButtonStr = "FF_BTN";
                            corrected = true;
                        } else if (cycleStartedBy == "FF_BTN" && correctionVolume <= config.datalog.ffToPfThreshold) {
                            cycleStartedBy = "PF_BTN";
                            if (!onlineLoggerMode) wakeUpButtonStr = "PF_BTN";
                            corrected = true;
                        }
                        
                        if (corrected) {
                            // Determine early snapshot pin states for log
                            int expectedState = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;
                            bool ffSnap = earlyGPIO_captured ? ((expectedState == HIGH) == (bool)((earlyGPIO_bitmask >> config.hardware.pinWakeupFF) & 1)) : false;
                            bool pfSnap = earlyGPIO_captured ? ((expectedState == HIGH) == (bool)((earlyGPIO_bitmask >> config.hardware.pinWakeupPF) & 1)) : false;
                            bool wifiSnap = earlyGPIO_captured ? ((expectedState == HIGH) == (bool)((earlyGPIO_bitmask >> config.hardware.pinWifiTrigger) & 1)) : false;
                            
                            // Write to btn_log.txt (same storage/path as datalog)
                            if (fsAvailable && activeFS) {
                                String folder = String(config.datalog.folder);
                                if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
                                if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
                                if (folder.length() == 0) folder = "/";
                                String btnLogPath = folder + "btn_log.txt";
                                
                                File btnLog = activeFS->open(btnLogPath, FILE_APPEND);
                                if (btnLog) {
                                    // Format: #:1234|bitmask:0x0008|early:FF=1,PF=0,WIFI=0|held:123ms|CORR:PF_BTN->FF_BTN|L:4.85
                                    char line[160];
                                    snprintf(line, sizeof(line), "#:%d|bitmask:0x%04X|early:FF=%d,PF=%d,WIFI=%d|held:%lums|CORR:%s->%s|L:%.2f",
                                        bootCount, earlyGPIO_bitmask,
                                        ffSnap, pfSnap, wifiSnap,
                                        buttonHeldMs,
                                        origButton.c_str(), cycleStartedBy.c_str(),
                                        correctionVolume);
                                    btnLog.println(line);
                                    btnLog.close();
                                    Serial.printf("BTN_LOG: %s\n", line);
                                }
                            }
                        }
                    }
                    
                    DBGF("Logging: FF:%d PF:%d Pulses:%lu Trigger:%s\n", 
                        highCountFF, highCountPF, currentPulses, cycleStartedBy.c_str());
                    addLogEntry();
                    flushLogBufferToFS();
                } else {
                    DBGLN("No activity to log.");
                }
                
                if (onlineLoggerMode) {
                    // Online Logger Mode: Reset for next cycle (NO SLEEP)
                    DBGLN("Online Logger: Resetting for next cycle");
                    noInterrupts();
                    cycleTotalPulses += pulseCount;
                    pulseCount = 0;
                    interrupts();
                    highCountFF = 0;
                    highCountPF = 0;
                    cycleStartedBy = "IDLE";
                    cycleButtonSet = false;
                    loggingState = STATE_IDLE;
                    stateStartTime = millis();
                    cycleStartTime = millis();
                    lastFlowPulseTime = 0;
                } else if (shouldRestart) {
                     // If restart pending, DON'T sleep!
                     // Just skip and let the loop reach the restart check at the beginning
                     DBGLN("Update pending, skipping sleep...");
                } else {
                    // Normal Mode: Enter deep sleep
                    configureWakeup();
                    DBGLN("Entering deep sleep...");
                    Serial.flush();
                    esp_deep_sleep_start();
                }
            }
            break;
    }
}