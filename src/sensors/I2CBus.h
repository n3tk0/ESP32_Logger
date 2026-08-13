// ============================================================================
// src/sensors/I2CBus.h
//
// Central registry for the chip's I2C controllers.
//
// WHY
// ---
// Before this, every I2C plugin called `Wire.begin(sda, scl)` in its own
// init() against the single global `Wire`. That had two consequences:
//
//   1. Only one bus was reachable, so two devices with the same fixed address
//      could never coexist. VEML6075 and VEML7700 are both hard-wired to 0x10
//      with no address-select pin, so that pair was simply impossible.
//
//   2. The last plugin to init won the pin assignment. Two sensors configured
//      with different sda/scl silently re-initialised the bus out from under
//      each other, and the first one just stopped answering — with no error
//      anywhere, because a re-begin() is not a failure.
//
// Both are fixed here: buses are claimed through acquire(), pins are recorded
// on first claim, and a later claim with conflicting pins is refused instead
// of silently reconfiguring the peripheral.
//
// HARDWARE LIMITS
// ---------------
// The number of I2C controllers is a property of the chip, not the board:
//
//   ESP32-S3 / ESP32   SOC_I2C_NUM = 2   → bus 0 and bus 1
//   ESP32-C3           SOC_I2C_NUM = 1   → bus 0 only
//
// The Arduino core defines the `Wire1` object unconditionally, so referring to
// it always links — but on a single-controller part `Wire1.begin()` fails
// inside the HAL (`i2c_num >= SOC_I2C_NUM`) and returns false with nothing but
// a log line. Rather than let that surface as a mystifying "sensor not found",
// acquire() rejects an out-of-range bus up front with an explicit message.
//
// This is why the check is a runtime one and not `#if SOC_I2C_NUM > 1`: the
// code compiles identically for every target, and a config carried over from
// an S3 to a C3 gets told exactly what is wrong.
//
// THREAD SAFETY
// -------------
// acquire()/resetAll() are called from sensor init, which runs under
// configMutex inside SensorManager::loadAndInit. The read-only accessors are
// safe to call from the web task: they read plain ints that only change during
// a config reload, and a torn read would at worst report a stale pin number in
// a diagnostic response.
//
// Bus ACCESS is serialised separately by wireMutex, which deliberately covers
// all buses with one lock rather than one lock each. Two controllers really
// can be driven concurrently, but the reads are short and infrequent (seconds
// apart) so the contention is irrelevant, and a single lock keeps /api/i2c_scan
// and the sensor tasks trivially correct against each other.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace I2CBus {

// Highest number of buses this firmware will manage, independent of the chip.
// The Arduino core exposes at most Wire and Wire1.
constexpr uint8_t MAX_BUSES = 2;

/// Number of I2C controllers actually usable on this chip (1 or 2).
uint8_t hardwareBusCount();

// ---------------------------------------------------------------------------
// acquire()
//   Claims `bus` for `who` with the given pins, initialising the peripheral on
//   the first claim. Returns the TwoWire to talk to, or nullptr on refusal:
//
//     • bus index beyond MAX_BUSES or beyond what the chip has
//     • sda/scl rejected by the active board profile
//     • bus already configured with DIFFERENT pins
//
//   Repeated claims with matching pins are the normal case — several sensors
//   share a bus — and return the same TwoWire without re-running begin().
//
//   `who` is a short identifier used only in log lines (the sensor's plugin
//   type or instance id).
// ---------------------------------------------------------------------------
TwoWire* acquire(uint8_t bus, int sda, int scl, const char* who);

/// The TwoWire for an already-configured bus, or nullptr.
TwoWire* get(uint8_t bus);

/// True once acquire() has successfully initialised `bus`.
bool isConfigured(uint8_t bus);

/// Pins a configured bus was brought up with; -1 when unconfigured.
int sdaOf(uint8_t bus);
int sclOf(uint8_t bus);

// ---------------------------------------------------------------------------
// resetAll()
//   Releases every configured bus (calling end() on the peripheral) and
//   forgets the recorded pins. Called from SensorManager::_destroyAll so a
//   config reload can re-assign pins; without it the old assignment would
//   outlive the sensors that requested it and refuse the new one.
//
//   MUST NOT run concurrently with bus traffic — callers hold configMutex,
//   and any path that ends a bus while a scan is in flight has to take
//   wireMutex too.
// ---------------------------------------------------------------------------
void resetAll();

}  // namespace I2CBus
