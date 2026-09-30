// ============================================================================
// src/utils/CpuFreq.h — the CPU frequency settings (config.hardware.cpuFreqMHz
// and config.hardware.activeCpuMHz), without Arduino, so the host tests can
// check them.
//
// The ESP32-S3 runs at 240 MHz; the ESP32-C3 tops out at 160. Below 80 MHz
// Wi-Fi stops working, so 80 is the floor for both.
//
// activeCpuMHz is the speed while the web server is up (Web Server / Online
// Logger modes, and continuous mode whenever the page is in use). It came out
// of HardwareConfig::reserved[], so a config from before it reads 0, which
// means 160 — what those modes always ran at.
// ============================================================================
#pragma once

#include <stdint.h>
#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>   // CONFIG_IDF_TARGET_*; absent on the host
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32)
constexpr int CPU_MAX_MHZ = 240;
#else
constexpr int CPU_MAX_MHZ = 160;
#endif

constexpr int CPU_ACTIVE_DEFAULT_MHZ = 160;

// 80, 160, or 240 where the chip can run it.
inline bool cpuMhzValid(int mhz, int maxMhz = CPU_MAX_MHZ) {
    return mhz == 80 || mhz == 160 || (mhz == 240 && maxMhz >= 240);
}

// The speed to set while the web server is up: the stored one, or for 0 (and
// anything this chip cannot run) the default.
inline uint32_t cpuActiveMhzFor(uint8_t stored, int maxMhz = CPU_MAX_MHZ) {
    return cpuMhzValid(stored, maxMhz) ? stored : CPU_ACTIVE_DEFAULT_MHZ;
}
