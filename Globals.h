#pragma once

#include "Config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>
#include <SD.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <FlowSensor.h>
#include <ESPAsyncWebServer.h>

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
extern DeviceConfig config;
extern ThreeWire*   rtcWire;
extern RtcDS1302<ThreeWire>* Rtc;
extern FlowSensor*  Sensor;
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
extern RTC_DATA_ATTR LogEntry logBuffer[20];   // LOG_BATCH_SIZE
extern RTC_DATA_ATTR int      logBufferCount;
extern RTC_DATA_ATTR int      bootCount;
extern RTC_DATA_ATTR bool     bootcount_restore;

// ============================================================================
// WAKE / CYCLE STATE
// ============================================================================
extern uint32_t currentWakeTimestamp;
extern String   wakeUpButtonStr;
extern String   cycleStartedBy;
extern bool     cycleButtonSet;
extern unsigned long cycleStartTime;
extern uint32_t cycleTotalPulses;

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
extern volatile uint32_t     pulseCount;
extern volatile unsigned long lastFFInterrupt;
extern volatile unsigned long lastPFInterrupt;
extern volatile unsigned long lastFlowInterrupt;
extern volatile bool          ffPressed;
extern volatile bool          pfPressed;
extern volatile bool          flowSensorPulseDetected;
extern volatile unsigned long isrDebounceUs;

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
// MISC
// ============================================================================
extern unsigned long lastLoggingCycleStartTime;
