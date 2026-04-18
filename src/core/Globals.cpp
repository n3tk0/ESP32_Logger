#include "Globals.h"

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
DeviceConfig config;
ThreeWire*   rtcWire = nullptr;
RtcDS1302<ThreeWire>* Rtc = nullptr;
AsyncWebServer server(80);

// ============================================================================
// STORAGE STATE
// ============================================================================
fs::FS* activeFS          = nullptr;
bool    sdAvailable       = false;
bool    littleFsAvailable = false;
bool    fsAvailable       = false;
String  currentStorageView = "internal";

// ============================================================================
// WIFI / NETWORK STATE
// ============================================================================
bool   apModeTriggered      = false;
bool   wifiConnectedAsClient = false;
bool   wifiFallbackToAP     = false;
bool   onlineLoggerMode     = false;
String currentIPAddress     = "";
String connectedSSID        = "";

// ============================================================================
// LOGGING BUFFER
// ============================================================================
RTC_DATA_ATTR LogEntry logBuffer[LOG_BATCH_SIZE];
RTC_DATA_ATTR int      logBufferCount   = 0;
RTC_DATA_ATTR int      bootCount        = 0;
RTC_DATA_ATTR bool     bootcount_restore = true;

// ============================================================================
// WAKE / CYCLE STATE
// ============================================================================
uint32_t     currentWakeTimestamp = 0;
String       wakeUpButtonStr      = "";
String       cycleStartedBy       = "BOOT";
bool         cycleButtonSet       = false;
unsigned long cycleStartTime      = 0;
volatile uint32_t cycleTotalPulses = 0;

uint32_t      earlyGPIO_bitmask = 0;
bool          earlyGPIO_captured = false;
unsigned long earlyGPIO_millis  = 0;
unsigned long buttonHeldMs      = 0;

// ============================================================================
// BUTTON DEBOUNCE STATE
// ============================================================================
int           highCountFF      = 0;
int           highCountPF      = 0;
int           stableFFState    = LOW;
int           stablePFState    = LOW;
unsigned long lastFFDebounceTime = 0;
unsigned long lastPFDebounceTime = 0;
int           lastFFButtonState = LOW;
int           lastPFButtonState = LOW;

// ============================================================================
// ISR STATE
// ============================================================================
volatile uint32_t     pulseCount            = 0;
volatile unsigned long lastFFInterrupt      = 0;
volatile unsigned long lastPFInterrupt      = 0;
volatile unsigned long lastFlowInterrupt    = 0;
volatile bool         ffPressed             = false;
volatile bool         pfPressed             = false;
volatile bool         flowSensorPulseDetected = false;
volatile uint32_t isrDebounceUs         = 100000;

// ============================================================================
// STATE MACHINE
// ============================================================================
LoggingState  loggingState    = STATE_IDLE;
unsigned long stateStartTime  = 0;
unsigned long lastFlowPulseTime = 0;

// ============================================================================
// SYSTEM FLAGS
// ============================================================================
bool          rtcValid      = false;
bool          shouldRestart = false;
unsigned long restartTimer  = 0;
String        statusMessage = "";
String        currentDir    = "/";

// ============================================================================
// MISC
// ============================================================================
unsigned long lastLoggingCycleStartTime = 0;

// ============================================================================
// PLATFORM SLEEP CONTROL
// ============================================================================
uint8_t g_sleepMode = 0;

// Web activity timestamp (C2 power restore)
volatile uint32_t g_lastWebActivity = 0;

// Deferred NTP sync — driven from loop() so /sync_time never blocks AsyncTCP
volatile uint8_t g_pendingNtpSync    = 0;
volatile int8_t  g_lastNtpSyncResult = 0;
