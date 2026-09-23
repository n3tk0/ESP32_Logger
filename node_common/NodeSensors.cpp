// ============================================================================
// node_common/NodeSensors.cpp — the runtime sensor layer (NodeSensors.h)
//
// Built by BOTH node firmwares through a one-line wrapper in each one's src/
// (PlatformIO compiles only what is under src/):
//
//   node/src/node_sensors_impl.cpp          ESP8266, Arduino core 3.x
//   node_espnow/src/<its wrapper>.cpp       ESP32-C3, arduino-esp32 2.x
//
// EVERY DRIVER IS COMPILED IN; ONLY THE LISTED ONES ARE BROUGHT UP
// ----------------------------------------------------------------
// The sensor set used to be NODE_SENSOR_* at build time (node/src/sensors.cpp
// before this file). It is now the `sensors` list of the applied config, so a
// node on a wall can be given a rain gauge by the collector. The drivers cost
// a few KB of flash between them, and nothing at all in RAM until an entry
// names them — the per-entry state below is a few bytes, the one big object
// (a DS18B20 bus: 8 ROM codes) is allocated only for a ds18b20 entry.
//
// The BMx280 / BME688 / DS18B20 drivers are the collector's, included
// unmodified from src/drivers/, so the compensation maths cannot drift between
// a wired sensor and a remote one. BH1750, SDS011 and the pulse counter are
// small enough to implement here, as node/ always did.
//
// PORTABILITY, THE PARTS THAT DIFFER
// ----------------------------------
//   SDS011 UART   ESP8266: SoftwareSerial (its only free hardware UART is the
//                 console). ESP32-C3: HardwareSerial(1), which the GPIO
//                 matrix routes to any pins.
//   1-Wire        DS18B20_Mini guards each bit with portDISABLE_INTERRUPTS,
//                 which the ESP8266 core lacks — shimmed below onto
//                 noInterrupts(), exactly as node/ has shipped it.
//   Pulse ISR     IRAM_ATTR on both. The counter is read under noInterrupts()
//                 on the ESP8266 and a portMUX spinlock on the ESP32, whose
//                 noInterrupts() is not a critical section.
//   I2C re-init   arduino-esp32 ignores Wire.begin() on a bus that is already
//                 up (it keeps the old pins), so a changed pair ends it first.
//
// RE-INIT ON A CHANGED CONFIG
// ---------------------------
// nodeSensorsBegin() with a different sensor set (or I2C pair) releases
// everything first — detachInterrupt, the serial port's end(), the 1-Wire bus
// objects — and only then brings the new set up. Attaching a second ISR to a
// pin that SoftwareSerial still owns is the kind of fault that compiles and
// only shows on hardware.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "NodeSensors.h"
#include "src/nodecfg/HwPins.h"

#include "src/drivers/BME280_Mini.h"
#include "src/drivers/BME688_Mini.h"

// DS18B20_Mini.h guards each bit of its 1-Wire timing with the FreeRTOS
// portDISABLE_INTERRUPTS / portENABLE_INTERRUPTS pair, which the ESP8266 core
// does not define. They map cleanly onto the Arduino pair it does provide —
// the driver only needs "nothing preempts this timed sequence".
//
// Shimmed here rather than changed in the driver: that file is the
// collector's, it is on a shipped code path, and portENTER/portEXIT have
// per-core semantics on the ESP32 that its author chose deliberately.
//
// Safe on this part despite WiFi: the critical sections are one bit each,
// roughly 70 µs, far short of the window where the ESP8266 starts dropping
// beacons. A whole-frame lock would not be.
#if defined(ESP8266)
#  ifndef portDISABLE_INTERRUPTS
#    define portDISABLE_INTERRUPTS() noInterrupts()
#  endif
#  ifndef portENABLE_INTERRUPTS
#    define portENABLE_INTERRUPTS()  interrupts()
#  endif
#endif
#include "src/drivers/DS18B20_Mini.h"

#if defined(ESP8266)
#  include <SoftwareSerial.h>
#else
#  include <HardwareSerial.h>
#endif

// Log lines with their format in flash: on the ESP8266 every string literal
// otherwise sits in RAM. arduino-esp32 maps printf_P and PSTR straight
// through, so the same line builds on both.
#define NS_LOG(fmt, ...) Serial.printf_P(PSTR(fmt), ##__VA_ARGS__)

using nodecfg::NodeConfig;
using nodecfg::SensorCfg;
using nodecfg::SensorType;
using nodecfg::PulseMode;
using nodecfg::MAX_SENSORS;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

/// What begin() did with one entry of the list. Indexed like cfg.sensors[],
/// and carries the type it was set up as, so a read against a list that
/// changed without a new begin() skips the entry instead of reading the wrong
/// device.
struct EntryState {
    SensorType    type  = SensorType::None;
    bool          ok    = false;
    uint8_t       addr  = 0;        ///< the I2C address that answered
    uint8_t       found = 0;        ///< ds18b20: probes on the bus
    DS18B20_Mini* ds    = nullptr;  ///< ds18b20: this entry's bus
};

/// The hardware part of one entry — everything but the ds18b20 metric name.
/// A renamed bus reads the same probes and needs no re-init.
struct HwSig {
    SensorType type;
    uint8_t    addr, pin, count, rx, tx;
    PulseMode  mode;
    float      per_pulse;
    uint32_t   debounce_us;
};

/// The hardware part of the config the current state was built from — what
/// decides whether a new begin() is a retry or a re-init. Zero-initialised
/// (so .bss, not .data: RAM either way, but no flash copy of an image).
struct Setup {
    uint8_t count;
    uint8_t sda;
    uint8_t scl;
    HwSig   sensors[MAX_SENSORS];
};

EntryState s_entry[MAX_SENSORS];
Setup      s_setup;
bool       s_haveSetup = false;
nodecfg::Hw s_hw       = nodecfg::Hw::Esp8266;
uint8_t    s_board     = 0;

// How a DS18B20 conversion is waited out (nodeSensorsSetWait); null = delay().
void (*s_waitConv)(uint32_t ms) = nullptr;

/// Wait for a sensor to finish measuring: the node's hook, else delay().
void waitMeasure(uint32_t ms) {
    if (s_waitConv) s_waitConv(ms);
    else            delay(ms);
}

/// One command byte to a BH1750. True when it ACKed.
bool bh1750Cmd(uint8_t addr, uint8_t cmd) {
    Wire.beginTransmission(addr);
    Wire.write(cmd);
    return Wire.endTransmission() == 0;
}

/// One measurement per read (see BH1750_CMD_ONCE_HRES) on a node that deep
/// sleeps between reads. A switch between sleep and mains always passes
/// through a restart or a deep sleep on the ESP-NOW node (main.cpp), so
/// begin() and read always agree on the mode.
bool bh1750OneShot(const NodeConfig& cfg) {
    return cfg.transport == nodecfg::Transport::EspNow && cfg.sleep;
}

// I2C: one bus shared by every I2C entry.
bool    s_wireUp  = false;
uint8_t s_wireSda = 0xFF;
uint8_t s_wireScl = 0xFF;

// The singletons: the validator allows at most one entry of each of these
// types (§1.2), so one driver object each is all there can be.
BME280_Mini s_bmx;
BME688_Mini s_bme688;

// BH1750 (ROHM BH1750FVI datasheet): high-resolution mode, 1 lx steps,
// 1.2 counts per lux at the default MTreg — matching the collector's BH1750
// plugin so the two agree. An H-resolution measurement takes 120 ms typical,
// 180 ms at most.
//
// Two ways to run it, chosen by bh1750OneShot():
//  * continuous (0x10), the collector's way: it measures for ever, so any
//    read returns a reading at most one measurement old. Right for a node
//    that stays awake, and what the WiFi node always did.
//  * one-time (0x20) on a sleeping ESP-NOW node: every nodeSensorsRead()
//    powers it on and starts ONE measurement, waited out together with any
//    DS18B20 conversion, after which the part powers itself down (0.01 uA
//    typical). Continuous mode would keep measuring at ~120 uA typical
//    (190 max) through every deep sleep — more than the whole board's
//    ~44 uA — and cost a 200 ms wait in begin() on every wake besides.
const uint8_t  BH1750_CMD_POWER_DOWN = 0x00;
const uint8_t  BH1750_CMD_POWER_ON   = 0x01;
const uint8_t  BH1750_CMD_CONT_HRES  = 0x10;
const uint8_t  BH1750_CMD_ONCE_HRES  = 0x20;
const uint32_t BH1750_HRES_MAX_MS    = 180;   // datasheet maximum
const float    BH1750_DIVIDER        = 1.2f;
/// One-shot: nodeSensorsRead() started a measurement and waited it out. One
/// flag, not one per entry: the validator allows a single bh1750 entry.
bool s_bhFresh = false;

// SDS011
#if defined(ESP8266)
SoftwareSerial s_sdsPort;
#else
HardwareSerial s_sdsPort(1);
#endif
bool    s_sdsUp   = false;
float   s_sdsPm25 = NAN;
float   s_sdsPm10 = NAN;
uint8_t s_sdsFrame[10];
uint8_t s_sdsPos  = 0;

// Pulse counter. Touched by the ISR, so volatile and read in a critical
// section.
volatile uint32_t s_pulses      = 0;
volatile uint32_t s_lastPulseUs = 0;
volatile uint32_t s_debounceUs  = 0;
int8_t            s_pulsePin    = -1;   ///< attached pin, -1 = none
uint32_t          s_lastDrainMs = 0;
float             s_pulseTotal  = 0.0f;
#if !defined(ESP8266)
portMUX_TYPE      s_pulseMux    = portMUX_INITIALIZER_UNLOCKED;
#endif

char s_describe[160];   ///< empty = "none"

}  // namespace

// Must live in IRAM on both chips: an ISR in flash faults the moment it fires
// while the flash cache is busy (a WiFi write, an NVS commit).
static void IRAM_ATTR pulseIsr() {
    const uint32_t now = micros();
#if !defined(ESP8266)
    portENTER_CRITICAL_ISR(&s_pulseMux);
#endif
    // Contact bounce on a reed switch arrives as a burst; anything closer
    // than the debounce window is the same tip. A flow sensor runs with 0.
    if (s_debounceUs == 0 || (uint32_t)(now - s_lastPulseUs) >= s_debounceUs) {
        s_lastPulseUs = now;
        s_pulses = s_pulses + 1;
    }
#if !defined(ESP8266)
    portEXIT_CRITICAL_ISR(&s_pulseMux);
#endif
}

/// Take and clear the count in one critical section, so a pulse arriving
/// mid-read is counted exactly once — in the next interval rather than
/// neither.
static uint32_t takePulses() {
#if defined(ESP8266)
    noInterrupts();
    const uint32_t n = s_pulses;
    s_pulses = 0;
    interrupts();
#else
    portENTER_CRITICAL(&s_pulseMux);
    const uint32_t n = s_pulses;
    s_pulses = 0;
    portEXIT_CRITICAL(&s_pulseMux);
#endif
    return n;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// "GPIO12 (D6)" — both ways of writing the same pin, because a wiring mistake
/// is the likeliest reason anyone is reading these lines, and the two
/// numbering schemes are exactly what people mix up.
static void describePin(char* out, size_t n, uint8_t gpio) {
    const char* d = nodecfg::pinLabel(s_hw, s_board, gpio);
    if (d && *d && strcmp(d, "") != 0) {
        // The SuperMini's labels ARE the GPIO numbers; "GPIO4 (4)" says nothing.
        char num[4];
        snprintf(num, sizeof(num), "%u", (unsigned)gpio);
        if (strcmp(d, num) != 0) {
            snprintf(out, n, "GPIO%u (%s)", (unsigned)gpio, d);
            return;
        }
    }
    snprintf(out, n, "GPIO%u", (unsigned)gpio);
}

/// A pin this chip cannot give a sensor. validate() refuses all of these; the
/// check is repeated because a config migrated from the old flat file is run
/// even when it would not validate today (see migrateLegacyWifi()), and a
/// driver must never be pointed at the flash bus.
static bool pinUnusable(uint8_t gpio) {
    return nodecfg::pinRisk(s_hw, gpio) == nodecfg::PIN_FORBIDDEN;
}

static HwSig sigOf(const SensorCfg& s) {
    HwSig h;
    h.type = s.type; h.addr = s.addr; h.pin = s.pin; h.count = s.count;
    h.rx = s.rx; h.tx = s.tx; h.mode = s.mode;
    h.per_pulse = s.per_pulse; h.debounce_us = s.debounce_us;
    return h;
}

/// Same hardware? Metric names are left out on purpose: a renamed ds18b20
/// bus reads the same probes, and needs no re-init. So is `per_pulse`: it is
/// a scale applied at read time from the live config, so a new one needs no
/// re-init — and comparing it with == would re-init (and zero the running
/// total) on the few-ULP drift a float picks up on its JSON round trips
/// between node, file and collector.
static bool sameSensor(const SensorCfg& a, const HwSig& b) {
    return a.type == b.type && a.addr == b.addr && a.pin == b.pin &&
           a.count == b.count && a.rx == b.rx && a.tx == b.tx &&
           a.mode == b.mode && a.debounce_us == b.debounce_us;
}

static bool sameSetup(const NodeConfig& cfg) {
    if (!s_haveSetup) return false;
    const uint8_t k = cfg.sensor_count <= MAX_SENSORS ? cfg.sensor_count : MAX_SENSORS;
    if (k != s_setup.count) return false;
    bool i2c = false;
    for (uint8_t i = 0; i < k; i++) {
        if (!sameSensor(cfg.sensors[i], s_setup.sensors[i])) return false;
        if (nodecfg::sensorIsI2c(cfg.sensors[i].type)) i2c = true;
    }
    if (i2c && (cfg.i2c.sda != s_setup.sda || cfg.i2c.scl != s_setup.scl)) return false;
    return true;
}

static void rememberSetup(const NodeConfig& cfg) {
    const uint8_t k = cfg.sensor_count <= MAX_SENSORS ? cfg.sensor_count : MAX_SENSORS;
    s_setup.count = k;
    s_setup.sda   = cfg.i2c.sda;
    s_setup.scl   = cfg.i2c.scl;
    for (uint8_t i = 0; i < k; i++) s_setup.sensors[i] = sigOf(cfg.sensors[i]);
    s_haveSetup = true;
}

/// Station pressure falls about 12 Pa per metre near sea level, so a node
/// 300 m up reads ~35 hPa below what a forecast quotes. Without the
/// conversion, comparing the node's reading against a forecast's figure
/// compares two different quantities.
static float toSeaLevel(float stationHpa, float tempC, float altitudeM) {
    if (altitudeM == 0.0f || !isfinite(stationHpa) || !isfinite(tempC)) return NAN;
    return stationHpa * powf(1.0f - (0.0065f * altitudeM)
                                    / (tempC + 0.0065f * altitudeM + 273.15f),
                             -5.257f);
}

static void appendDescribe(const char* s) {
    const size_t used = strlen(s_describe);
    if (used + 1 >= sizeof(s_describe)) return;
    strncat(s_describe, s, sizeof(s_describe) - used - 1);
}

// ---------------------------------------------------------------------------
// Tear down
// ---------------------------------------------------------------------------

static void releaseAll() {
    if (s_pulsePin >= 0) {
        detachInterrupt(digitalPinToInterrupt((uint8_t)s_pulsePin));
        s_pulsePin = -1;
    }
    if (s_sdsUp) {
        s_sdsPort.end();
        s_sdsUp = false;
    }
    for (uint8_t i = 0; i < MAX_SENSORS; i++) {
        delete s_entry[i].ds;
        s_entry[i] = EntryState();
    }
    s_sdsPm25 = NAN;
    s_sdsPm10 = NAN;
    s_sdsPos  = 0;
    s_pulses  = 0;
    s_pulseTotal = 0.0f;
    s_bhFresh   = false;
    s_haveSetup = false;
}

void nodeSensorsEnd() {
    // A BH1750 left in continuous mode measures through a deep sleep at
    // ~120 uA; one-shot ones are already down, and a second 0x00 is harmless.
    for (uint8_t i = 0; i < MAX_SENSORS; i++)
        if (s_wireUp && s_entry[i].ok && s_entry[i].type == SensorType::Bh1750)
            bh1750Cmd(s_entry[i].addr, BH1750_CMD_POWER_DOWN);
    releaseAll();
#if !defined(ESP8266)
    if (s_wireUp) Wire.end();
#endif
    s_wireUp = false;
    s_wireSda = s_wireScl = 0xFF;
    s_describe[0] = '\0';
}

// ---------------------------------------------------------------------------
// Bring-up, one entry at a time
// ---------------------------------------------------------------------------

static bool ensureWire(const NodeConfig& cfg) {
    if (pinUnusable(cfg.i2c.sda) || pinUnusable(cfg.i2c.scl)) {
        NS_LOG("[sensor] I2C on GPIO%u/GPIO%u refused: flash bus\n",
                      (unsigned)cfg.i2c.sda, (unsigned)cfg.i2c.scl);
        return false;
    }
    if (s_wireUp && s_wireSda == cfg.i2c.sda && s_wireScl == cfg.i2c.scl) return true;
#if !defined(ESP8266)
    if (s_wireUp) Wire.end();
#endif
    Wire.begin((int)cfg.i2c.sda, (int)cfg.i2c.scl);
    s_wireUp  = true;
    s_wireSda = cfg.i2c.sda;
    s_wireScl = cfg.i2c.scl;
    // Printed on every bring-up. "No sensor found" and "no sensor found ON
    // THESE TWO PINS" are the same sentence to the firmware and very different
    // ones to whoever is holding the board: the commonest cause is a
    // silkscreen D-number entered as a GPIO, and this line is where it shows.
    char sda[20], scl[20];
    describePin(sda, sizeof(sda), cfg.i2c.sda);
    describePin(scl, sizeof(scl), cfg.i2c.scl);
    NS_LOG("[sensor] I2C on SDA=%s SCL=%s\n", sda, scl);
    return true;
}

/// Breakouts strap the address either way; try the configured one first and
/// then the other, so a board that shipped as 0x77 works untouched. addr 0
/// ("probe both") starts at 0x76, as node/ always did.
static void addrCandidates(uint8_t addr, uint8_t a, uint8_t b, uint8_t out[2]) {
    out[0] = (addr == b) ? b : a;
    out[1] = (out[0] == a) ? b : a;
}

static bool beginBmx(EntryState& e, const SensorCfg& s, const NodeConfig& cfg) {
    uint8_t cand[2];
    addrCandidates(s.addr, 0x76, 0x77, cand);
    for (uint8_t i = 0; i < 2; i++) {
        if (s_bmx.begin(cand[i], &Wire)) {
            e.addr = cand[i];
            NS_LOG("[sensor] %s at 0x%02X\n", s_bmx.isBME280() ? "BME280" : "BMP280",
                          cand[i]);
            return true;
        }
    }
    char sda[20], scl[20];
    describePin(sda, sizeof(sda), cfg.i2c.sda);
    describePin(scl, sizeof(scl), cfg.i2c.scl);
    NS_LOG("[sensor] no BME280/BMP280 at 0x%02X or 0x%02X (SDA=%s SCL=%s)\n",
                  cand[0], cand[1], sda, scl);
    return false;
}

static bool beginBme688(EntryState& e, const SensorCfg& s, const NodeConfig& cfg) {
    uint8_t cand[2];
    addrCandidates(s.addr, 0x76, 0x77, cand);
    for (uint8_t i = 0; i < 2; i++) {
        if (s_bme688.begin(cand[i], &Wire)) {
            e.addr = cand[i];
            NS_LOG("[sensor] BME688 at 0x%02X\n", cand[i]);
            return true;
        }
    }
    char sda[20], scl[20];
    describePin(sda, sizeof(sda), cfg.i2c.sda);
    describePin(scl, sizeof(scl), cfg.i2c.scl);
    NS_LOG("[sensor] no BME680/BME688 at 0x%02X or 0x%02X (SDA=%s SCL=%s)\n",
                  cand[0], cand[1], sda, scl);
    return false;
}

static bool beginBh1750(EntryState& e, const SensorCfg& s, const NodeConfig& cfg) {
    // ADDR low is 0x23, ADDR high 0x5C. The configured one first, then the
    // other, for the same reason as the BMx280.
    uint8_t cand[2];
    addrCandidates(s.addr, 0x23, 0x5C, cand);
    const bool once = bh1750OneShot(cfg);
    for (uint8_t i = 0; i < 2; i++) {
        if (once) {
            // Presence only, and the state it should sleep in: the read
            // powers it on and measures. No wait here at all.
            if (!bh1750Cmd(cand[i], BH1750_CMD_POWER_DOWN)) continue;
        } else {
            if (!bh1750Cmd(cand[i], BH1750_CMD_POWER_ON)) continue;
            if (!bh1750Cmd(cand[i], BH1750_CMD_CONT_HRES)) continue;
            // The first continuous measurement: until it completes the data
            // register holds nothing worth reading.
            waitMeasure(BH1750_HRES_MAX_MS + 20);
        }
        e.addr    = cand[i];
        s_bhFresh = false;
        if (once) NS_LOG("[sensor] BH1750 at 0x%02X, one-shot\n", cand[i]);
        else      NS_LOG("[sensor] BH1750 at 0x%02X, continuous\n", cand[i]);
        return true;
    }
    char sda[20], scl[20];
    describePin(sda, sizeof(sda), cfg.i2c.sda);
    describePin(scl, sizeof(scl), cfg.i2c.scl);
    NS_LOG("[sensor] no BH1750 at 0x%02X or 0x%02X (SDA=%s SCL=%s)\n",
                  cand[0], cand[1], sda, scl);
    return false;
}

static bool beginDs(EntryState& e, const SensorCfg& s) {
    char pin[20];
    describePin(pin, sizeof(pin), s.pin);
    if (pinUnusable(s.pin)) {
        NS_LOG("[sensor] DS18B20 on %s refused: flash bus\n", pin);
        return false;
    }
    if (!e.ds) e.ds = new DS18B20_Mini();
    if (!e.ds) return false;
    if (!e.ds->begin(s.pin)) {
        NS_LOG("[sensor] no DS18B20 on %s (4.7k pull-up to 3V3 fitted?)\n", pin);
        e.found = 0;
        return false;
    }
    e.found = (uint8_t)e.ds->deviceCount();
    NS_LOG("[sensor] DS18B20 x%u on %s (%u expected)\n",
                  (unsigned)e.found, pin, (unsigned)s.count);
    return true;
}

static bool beginSds(const SensorCfg& s) {
    if (pinUnusable(s.rx) || pinUnusable(s.tx) ||
        !nodecfg::pinHasInterrupt(s_hw, s.rx)) {
        NS_LOG("[sensor] SDS011 on RX=GPIO%u TX=GPIO%u refused\n",
                      (unsigned)s.rx, (unsigned)s.tx);
        return false;
    }
    if (!s_sdsUp) {
#if defined(ESP8266)
        s_sdsPort.begin(9600, SWSERIAL_8N1, s.rx, s.tx, false);
#else
        s_sdsPort.begin(9600, SERIAL_8N1, s.rx, s.tx);
#endif
        s_sdsUp = true;
    }
    // No handshake to confirm: the SDS011 simply streams a frame a second
    // once powered. The port being up is "ok"; whether a valid frame arrives
    // decides whether pm25/pm10 are published.
    char rx[20], tx[20];
    describePin(rx, sizeof(rx), s.rx);
    describePin(tx, sizeof(tx), s.tx);
    NS_LOG("[sensor] SDS011 listening on RX=%s TX=%s\n", rx, tx);
    return true;
}

static bool beginPulse(const SensorCfg& s) {
    char pin[20];
    describePin(pin, sizeof(pin), s.pin);
    if (pinUnusable(s.pin) || !nodecfg::pinHasInterrupt(s_hw, s.pin)) {
        NS_LOG("[sensor] pulse input on %s refused: no interrupt\n", pin);
        return false;
    }
    if (s_pulsePin < 0) {
        s_debounceUs = s.debounce_us;
        pinMode(s.pin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(s.pin), pulseIsr, FALLING);
        s_pulsePin    = (int8_t)s.pin;
        s_lastDrainMs = millis();
    }
    NS_LOG("[sensor] pulse input on %s (%s, %g per pulse, %lu us debounce)\n",
                  pin, nodecfg::pulseModeName(s.mode), (double)s.per_pulse,
                  (unsigned long)s.debounce_us);
    return true;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

int nodeSensorsBegin(const NodeConfig& cfg) {
    s_hw    = cfg.hw;
    s_board = cfg.board;

    if (!sameSetup(cfg)) {
        // A different set: release every interrupt, port and bus object the
        // old one held BEFORE anything new claims a pin.
        releaseAll();
        rememberSetup(cfg);
    }

    const uint8_t k = s_setup.count;
    bool wireOk = true, wireTried = false;
    int ok = 0;
    for (uint8_t i = 0; i < k; i++) {
        const SensorCfg& s = cfg.sensors[i];
        EntryState& e = s_entry[i];
        e.type = s.type;
        if (e.ok) { ok++; continue; }   // answered before: nothing to retry

        if (nodecfg::sensorIsI2c(s.type)) {
            if (!wireTried) { wireOk = ensureWire(cfg); wireTried = true; }
            if (!wireOk) continue;
        }
        switch (s.type) {
            case SensorType::Bmx280:  e.ok = beginBmx(e, s, cfg);    break;
            case SensorType::Bme688:  e.ok = beginBme688(e, s, cfg); break;
            case SensorType::Bh1750:  e.ok = beginBh1750(e, s, cfg); break;
            case SensorType::Ds18b20: e.ok = beginDs(e, s);          break;
            case SensorType::Sds011:  e.ok = beginSds(s);            break;
            case SensorType::Pulse:   e.ok = beginPulse(s);          break;
            default: break;
        }
        if (e.ok) ok++;
    }

    // "bmx280@0x76 ok, ds18b20x2@GPIO12 ok, pulse@GPIO4 rain"
    s_describe[0] = '\0';
    for (uint8_t i = 0; i < k; i++) {
        const SensorCfg& s = cfg.sensors[i];
        const EntryState& e = s_entry[i];
        char part[40];
        const char* tn = nodecfg::sensorTypeName(s.type);
        switch (s.type) {
            case SensorType::Bmx280:
            case SensorType::Bme688:
            case SensorType::Bh1750:
                snprintf(part, sizeof(part), "%s@0x%02X %s", tn,
                         (unsigned)(e.ok ? e.addr : s.addr), e.ok ? "ok" : "missing");
                break;
            case SensorType::Ds18b20:
                snprintf(part, sizeof(part), "%sx%u@GPIO%u %s", tn, (unsigned)e.found,
                         (unsigned)s.pin, e.ok ? "ok" : "missing");
                break;
            case SensorType::Sds011:
                snprintf(part, sizeof(part), "%s@GPIO%u %s", tn, (unsigned)s.rx,
                         e.ok ? "ok" : "refused");
                break;
            case SensorType::Pulse:
                snprintf(part, sizeof(part), "%s@GPIO%u %s", tn, (unsigned)s.pin,
                         e.ok ? nodecfg::pulseModeName(s.mode) : "refused");
                break;
            default:
                snprintf(part, sizeof(part), "?");
                break;
        }
        if (i) appendDescribe(", ");
        appendDescribe(part);
    }
    return ok;
}

bool nodeSensorsReady() {
    for (uint8_t i = 0; i < s_setup.count && i < MAX_SENSORS; i++)
        if (s_entry[i].ok) return true;
    return false;
}

const char* nodeSensorsDescribe() { return s_describe[0] ? s_describe : "none"; }

void nodeSensorsSetWait(void (*wait)(uint32_t ms)) { s_waitConv = wait; }

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/// Drain whatever the SDS011 sent since the last read and keep the newest
/// complete frame. It streams at 1 Hz and a node reads once a minute, so the
/// buffer holds many; the latest is the one to send. Frame: AA C0 pm25L pm25H
/// pm10L pm10H id1 id2 sum AB, values in tenths of a ug/m3 — matching the
/// collector's SDS011 plugin.
static void drainSds() {
    while (s_sdsPort.available()) {
        const uint8_t b = (uint8_t)s_sdsPort.read();
        if (s_sdsPos == 0 && b != 0xAA) continue;
        s_sdsFrame[s_sdsPos++] = b;
        if (s_sdsPos < sizeof(s_sdsFrame)) continue;
        s_sdsPos = 0;
        if (s_sdsFrame[1] != 0xC0 || s_sdsFrame[9] != 0xAB) continue;
        uint8_t sum = 0;
        for (int i = 2; i <= 7; i++) sum += s_sdsFrame[i];
        if (sum != s_sdsFrame[8]) continue;          // corrupt frame, drop it
        s_sdsPm25 = (float)((s_sdsFrame[3] << 8) | s_sdsFrame[2]) / 10.0f;
        s_sdsPm10 = (float)((s_sdsFrame[5] << 8) | s_sdsFrame[4]) / 10.0f;
    }
}

/// One entry's values, in the order sensorTypeMetricIds() lists them (for a
/// ds18b20, probe by probe). NAN for anything it could not produce.
static uint8_t readEntry(uint8_t si, const SensorCfg& s, float altitude, bool bhOnce, float* v,
                         uint8_t cap) {
    EntryState& e = s_entry[si];
    for (uint8_t i = 0; i < cap; i++) v[i] = NAN;
    switch (s.type) {
        case SensorType::Bmx280: {
            const float t  = s_bmx.readTemperature();
            const float pa = s_bmx.readPressure();
            const float hPa = isfinite(pa) ? pa / 100.0f : NAN;
            v[0] = t;
            v[1] = s_bmx.readHumidity();   // NAN on a BMP280
            v[2] = hPa;
            v[3] = toSeaLevel(hPa, t, altitude);
            return 4;
        }
        case SensorType::Bme688: {
            if (!s_bme688.performReading()) return 5;
            const float hPa = s_bme688.pressure / 100.0f;
            v[0] = s_bme688.temperature;
            v[1] = s_bme688.humidity;
            v[2] = hPa;
            v[3] = toSeaLevel(hPa, s_bme688.temperature, altitude);
            // 0 is the driver's "no valid gas reading", not zero ohms.
            v[4] = s_bme688.gas_resistance > 0.0f ? s_bme688.gas_resistance : NAN;
            return 5;
        }
        case SensorType::Bh1750: {
            // One-shot: only a measurement nodeSensorsRead() started (and
            // waited out) this time. Otherwise the register holds the last
            // wake's value, or nothing after a power-on.
            if (bhOnce) {
                if (!s_bhFresh) return 1;
                s_bhFresh = false;
            }
            Wire.requestFrom((uint8_t)e.addr, (uint8_t)2);
            if (Wire.available() >= 2) {
                // Two separate statements on purpose: `(read() << 8) | read()`
                // leaves the operand order unsequenced, and a right-first
                // evaluation swaps the bytes into a value that still looks
                // like a plausible lux reading.
                const uint8_t hi = (uint8_t)Wire.read();
                const uint8_t lo = (uint8_t)Wire.read();
                v[0] = (float)(((uint16_t)hi << 8) | lo) / BH1750_DIVIDER;
            }
            return 1;
        }
        case SensorType::Sds011:
            drainSds();
            v[0] = s_sdsPm25;
            v[1] = s_sdsPm10;
            return 2;
        case SensorType::Pulse: {
            const uint32_t pulses  = takePulses();
            const uint32_t nowMs   = millis();
            const uint32_t elapsed = nowMs - s_lastDrainMs;   // wrap-safe
            s_lastDrainMs = nowMs;
            const float units = (float)pulses * s.per_pulse;
            s_pulseTotal += units;
            // Rate as an average over the interval just ended, not
            // extrapolated from the gap between the last two pulses: one tip
            // near the deadline is not a downpour.
            float rate = NAN;
            if (elapsed > 0) {
                rate = (s.mode == PulseMode::Rain)
                           ? units * 3600000.0f / (float)elapsed    // mm per hour
                           : units * 60000.0f / (float)elapsed;     // litres per minute
            }
            v[0] = rate;
            v[1] = s_pulseTotal;
            return 2;
        }
        case SensorType::Ds18b20: {
            // The probes were asked to convert by nodeSensorsRead(), which
            // waited out the conversion once for every bus.
            const uint8_t n = s.count < cap ? s.count : cap;
            for (uint8_t p = 0; p < n && p < e.found; p++) {
                const float t = e.ds->getTempC(p);
                // The driver reports a missing probe as -127, which is a
                // plausible-looking float the collector would happily store.
                if (t > DS18B20_Mini::DISCONNECTED + 0.5f) v[p] = t;
            }
            return n;
        }
        default:
            return 0;
    }
}

int nodeSensorsRead(const NodeConfig& cfg, NodeReading* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;

    nodecfg::MetricSlot slots[nodecfg::MAX_METRIC_SLOTS];
    const uint8_t total = nodecfg::listMetrics(cfg, slots, nodecfg::MAX_METRIC_SLOTS, false);
    const uint8_t nslots = total < nodecfg::MAX_METRIC_SLOTS ? total : nodecfg::MAX_METRIC_SLOTS;

    const uint8_t k = cfg.sensor_count <= MAX_SENSORS ? cfg.sensor_count : MAX_SENSORS;

    // Every 1-Wire bus converts at once, and the wait is paid once. The
    // driver's requestTemperatures() does not wait, and reading straight after
    // it returns the PREVIOUS conversion — 85 °C, the power-on value, on the
    // first cycle after boot. The collector's DS18B20 plugin waits; so does
    // this — through the node's hook when it has one (NodeSensors.h: the
    // ESP-NOW node light-sleeps it on a battery), else with delay().
    //
    // A one-shot BH1750 (see BH1750_CMD_ONCE_HRES) is started here too, so a
    // node with both waits once, for the longer of the two.
    uint32_t convMs = 0;
    const bool bhOnce = bh1750OneShot(cfg);
    for (uint8_t i = 0; i < k; i++) {
        EntryState& e = s_entry[i];
        if (e.type != cfg.sensors[i].type || !e.ok) continue;
        if (e.type == SensorType::Ds18b20) {
            if (!e.ds || e.found == 0) continue;
            e.ds->requestTemperatures();
            if (e.ds->conversionTimeMs() > convMs) convMs = e.ds->conversionTimeMs();
        } else if (e.type == SensorType::Bh1750 && bhOnce) {
            s_bhFresh = bh1750Cmd(e.addr, BH1750_CMD_POWER_ON) &&
                        bh1750Cmd(e.addr, BH1750_CMD_ONCE_HRES);
            if (s_bhFresh && BH1750_HRES_MAX_MS > convMs) convMs = BH1750_HRES_MAX_MS;
        }
    }
    if (convMs) waitMeasure(convMs);

    int n = 0;
    uint8_t si = 0;
    uint8_t slot = 0;
    for (; si < k; si++) {
        const SensorCfg& s = cfg.sensors[si];
        const EntryState& e = s_entry[si];
        // Skip an entry that is not the one begin() set up (the list changed
        // without a new nodeSensorsBegin()) or that is not answering.
        const bool live = (e.type == s.type) && e.ok;

        float v[nodecfg::DS_MAX_COUNT];
        uint8_t nv = 0;
        if (live)
            nv = readEntry(si, s, cfg.altitude_m, bhOnce, v, (uint8_t)(sizeof(v) / sizeof(v[0])));

        // The slots of this entry, in order: the k-th of them is v[k].
        uint8_t j = 0;
        while (slot < nslots && slots[slot].sensor == si) {
            const nodecfg::MetricSlot& m = slots[slot];
            slot++;
            const uint8_t idx = j++;
            if (!live || idx >= nv) continue;
            if (m.needsAltitude && cfg.altitude_m == 0.0f) continue;
            if (!isfinite(v[idx])) continue;   // omit rather than send a NaN
            if (n >= maxOut) continue;
            NodeReading& r = out[n++];
            r.metricId = m.id;
            r.index    = m.index;
            r.value    = v[idx];
            memcpy(r.name, m.name, sizeof(r.name));
            r.name[sizeof(r.name) - 1] = '\0';
            r.unit     = m.unit;
        }
    }
    return n;
}
