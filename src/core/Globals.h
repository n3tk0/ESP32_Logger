#pragma once

#include "Config.h"
#include <Arduino.h>
// Header-bloat audit: <FS.h> stays (fs::FS is referenced by many consumers of
// this central header), but the heavy concrete FS backends <LittleFS.h> and
// <SD.h> are not needed here — only the `extern fs::FS* activeFS` pointer and
// bool availability flags are declared.  Files that touch the LittleFS / SD
// global objects include those headers directly.
#include <FS.h>
#include <atomic>
#include "../drivers/DS1302_Mini.h"
#include <ESPAsyncWebServer.h>

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
extern DeviceConfig config;
extern ThreeWire*   rtcWire;
extern RtcDS1302<ThreeWire>* Rtc;
extern AsyncWebServer server;

// ============================================================================
// STORAGE STATE
// ============================================================================
extern fs::FS* activeFS;
extern bool    sdAvailable;
extern bool    littleFsAvailable;
extern bool    fsAvailable;
extern String  currentStorageView;

// ============================================================================
// WIFI / NETWORK STATE
// ============================================================================
extern bool   apModeTriggered;
extern bool   wifiConnectedAsClient;
extern bool   wifiFallbackToAP;
extern bool   onlineLoggerMode;
extern String currentIPAddress;
extern String connectedSSID;

// ============================================================================
// LOGGING BUFFER (RTC_DATA_ATTR survives deep sleep)
// ============================================================================
extern RTC_DATA_ATTR LogEntry logBuffer[LOG_BATCH_SIZE];
extern RTC_DATA_ATTR int      logBufferCount;
extern RTC_DATA_ATTR int      bootCount;
extern RTC_DATA_ATTR bool     bootcount_restore;

// ============================================================================
// WAKE / CYCLE STATE
// ============================================================================
extern uint32_t currentWakeTimestamp;
// R14 / AUDIT 6.3: char[] instead of String. These are mutated from
// loop() and read by AsyncTCP workers — a String buffer pointer
// triple is not safe across that cross-task boundary. Fixed char
// arrays mean reads see at-worst a partially-updated byte sequence,
// not a freed pointer. Writers use strncpy + explicit null-terminate;
// readers compare with strcmp.
extern char     wakeUpButtonStr[16];
extern char     cycleStartedBy[16];
extern bool     cycleButtonSet;
extern unsigned long cycleStartTime;
extern volatile uint32_t cycleTotalPulses;

// Early GPIO snapshot
extern uint32_t      earlyGPIO_bitmask;
extern bool          earlyGPIO_captured;
extern unsigned long earlyGPIO_millis;
extern unsigned long buttonHeldMs;

// ============================================================================
// BUTTON DEBOUNCE STATE
// ============================================================================
extern int           highCountFF;
extern int           highCountPF;
extern int           stableFFState, stablePFState;
extern unsigned long lastFFDebounceTime, lastPFDebounceTime;
extern int           lastFFButtonState, lastPFButtonState;

// ============================================================================
// ISR STATE
// ============================================================================
// R28 / AUDIT 2.13 + 6.4: std::atomic<uint32_t> — flow ISR increments racing
// loop()/web reads. fetch_add(relaxed) on RISC-V emits an aligned word add;
// exchange(0) in loop() replaces the noInterrupts/interrupts read-clear pair.
extern std::atomic<uint32_t> pulseCount;
// R12 / AUDIT 1.5: lastFFInterrupt / lastPFInterrupt / ffPressed / pfPressed
// removed with the dead onFFButton / onPFButton ISRs (HardwareManager.cpp).
extern volatile unsigned long lastFlowInterrupt;
extern volatile bool          flowSensorPulseDetected;

// ============================================================================
// STATE MACHINE
// ============================================================================
extern LoggingState  loggingState;
extern unsigned long stateStartTime;
extern unsigned long lastFlowPulseTime;

// ============================================================================
// SYSTEM FLAGS
// ============================================================================
extern bool          rtcValid;
extern bool          shouldRestart;
extern unsigned long restartTimer;
extern String        statusMessage;
extern String        currentDir;

// ============================================================================
// PLATFORM MODE
// ============================================================================
// Detected at boot from /platform_config.json.  Owned in Globals.cpp; the
// sketch initialises it from _detectPlatformMode() during setup().  Other
// translation units (TaskManager, StorageTask, FlowRunLogger) read it to
// pick the appropriate storage / flow-logging path.
extern PlatformMode g_platformMode;

// ============================================================================
// BOARD PROFILE (R11 — pin-rules registry, AUDIT 5.x / 23.1 / 31.x family)
// ----------------------------------------------------------------------------
// Loaded once at boot from /board_profile.txt.  nullptr means no profile
// is selected — the first-run wizard must run before pin assignments can
// be validated. See src/core/BoardProfiles.h for the validator API.
// ============================================================================
struct BoardProfile;
extern const BoardProfile* g_boardProfile;

// True if the first-run wizard must complete before normal operation.
// Set during setup() if g_boardProfile == nullptr; cleared when the user
// successfully POSTs /api/firstrun. While true, the web server redirects
// all non-wizard routes to /firstrun.
extern bool g_setupRequired;

// ============================================================================
// PLATFORM SLEEP CONTROL
// ============================================================================
// 0 = deep sleep allowed (legacy default)
// 1 = light/modem sleep only
// 2 = no sleep (continuous, web server active)
extern uint8_t g_sleepMode;

// ============================================================================
// MISC
// ============================================================================
extern unsigned long lastLoggingCycleStartTime;

// Web activity timestamp — updated by web request handlers (C2 power restore)
extern volatile uint32_t g_lastWebActivity;

// ============================================================================
// DEFERRED ACTIONS (executed from loop() so async handlers don't block)
// ============================================================================
#include <atomic>
// g_pendingNtpSync:      0 = idle, 1 = requested, 2 = running
// g_lastNtpSyncResult:   0 = unknown, 1 = ok, -1 = fail
extern volatile uint8_t g_pendingNtpSync;
extern volatile int8_t  g_lastNtpSyncResult;

// g_pendingWiFiShutdown: set by async handlers that need safeWiFiShutdown();
//                        loop() calls it so AsyncTCP worker is never blocked.
extern volatile bool g_pendingWiFiShutdown;

// g_pendingOtaRollback / g_pendingRtcSet: std::atomic<bool> so writer (AsyncTCP
// task) and reader (main loop, potentially on a different core) see consistent
// state without relying on volatile alone.  release-store pairs with
// acquire-load so any data written before the flag (g_pendingRtcTime struct)
// is visible to the reader.  (AUDIT 3.16 / 3.17)
extern std::atomic<bool> g_pendingOtaRollback;

// g_pendingRtcSet: /set_time updates the POSIX clock immediately (non-blocking)
// then sets this flag so loop() performs the hardware RTC writes (3 × delay)
// without blocking the AsyncTCP worker.  (AUDIT 3.17)
struct PendingRtcSet {
    uint16_t year;
    uint8_t  month, day, hour, minute;
};
extern std::atomic<bool> g_pendingRtcSet;
extern PendingRtcSet     g_pendingRtcTime;  // written before release-store above

// ============================================================================
// RESTART CIRCUIT BREAKER (Pillar 3.7 / AUDIT FC.4)
// ----------------------------------------------------------------------------
// Counts consecutive non-graceful resets in RTC slow memory. If 3+ resets
// happen without a 60-second healthy uptime in between, setup() enters
// SAFE_MODE: skips _initPlatform / legacy state-machine, brings up WiFi AP
// + web server only, so the user can OTA-recover.
//
// RESET_GUARD_MAGIC distinguishes a real warm boot (magic preserved) from a
// cold boot where RTC slow memory contents are undefined.
// ============================================================================
constexpr uint32_t RESET_GUARD_MAGIC = 0x57415445;  // "WATE"

extern RTC_DATA_ATTR uint32_t g_resetMagic;
extern RTC_DATA_ATTR uint32_t g_consecutiveResets;
extern bool                    g_safeMode;
