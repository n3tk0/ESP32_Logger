// ============================================================================
// src/nodecfg/NodeConfig.h
//
// A sensor node's settings, as one plain struct that three firmwares share.
//
// docs/NODE_CONFIG.md §1 is the contract; this is that document with fixed
// sizes. The WiFi node (node/, ESP8266), the ESP-NOW node (node_espnow/, XIAO
// ESP32-C3) and the collector (src/) all compile THIS header, and the JSON
// codec, the validator and the metric catalogue next to it — so the page on
// the node, the page on the collector and the node itself cannot disagree
// about what a setting means or whether it is allowed. The alternative is
// three hand-written parsers, and a node that accepts a config the collector
// refused, or the other way round, with every side looking right on its own.
//
// WHY FIXED-SIZE FIELDS AND NOT std::string / JsonDocument
// --------------------------------------------------------
// The ESP8266 has about 40 KB of heap left once WiFi and TLS-less HTTP are up,
// and it fragments. A struct with char arrays is one allocation (or none, on
// the stack), has a size the compiler can print, and copies with `=`. It is
// also what lets the validator be a pure function with no allocator in it,
// which is what makes it testable on the host in the first place. The sizes
// are the protocol maxima (an SSID is at most 32 bytes, a WPA2 passphrase 63)
// or the collector's own limits (a node name becomes SensorReading::sensorId,
// which is char[17] on the collector), not guesses.
//
// NO ARDUINO HERE
// ---------------
// Nothing in this header, MetricCatalog.h, HwPins.h or NodeConfigValidate.h
// includes Arduino.h, and that is deliberate: tests/host compiles them with a
// desktop g++. Only NodeConfigJson.h needs a library (ArduinoJson v7), and the
// host tests get that from tests/host/vendor/.
//
// C++11. The collector and the ESP-NOW node build with -std=gnu++11 (the
// arduino-esp32 2.x default), so nothing here may use C++14 or later.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace nodecfg {

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

/// Sensor entries in one config. Also the metric budget below — a list of
/// eight entries that each produced one metric is the most that could fit.
static const uint8_t MAX_SENSORS = 8;

/// Metrics a node may publish per reading, counted over every sensor entry.
///
/// This is the collector's MAX_METRICS_PER_TICK (src/sensors/SensorManager.cpp)
/// and it is a hard limit there: RemoteIngest hands at most that many metrics
/// of one node to the pipeline per tick, and the ninth is dropped without an
/// error anywhere. So the validator refuses a config that would produce a
/// ninth rather than let the node publish one nobody will ever see.
static const uint8_t MAX_METRICS = 8;

/// A node name: the WiFi node's ingest id (the collector clamps ids to 16
/// characters), and the label an ESP-NOW node shows on the collector.
static const uint8_t NODE_NAME_MAX = 16;

/// The DS18B20 `metric` base name. SensorReading::metric is char[16] on the
/// collector; the multi-probe suffix "_7" needs two of the fifteen usable
/// characters, and ten leaves room to spare.
static const uint8_t DS_METRIC_MAX = 10;

/// Probes one DS18B20 entry may declare.
static const uint8_t DS_MAX_COUNT = 8;

static const uint16_t INTERVAL_MIN_S = 10;
static const uint16_t INTERVAL_MAX_S = 65535;

// String capacities, terminator included.
static const size_t SSID_CAP   = 33;   ///< 802.11: at most 32 bytes
static const size_t PASS_CAP   = 65;   ///< WPA2: 8..63 chars, or 64 hex
static const size_t HOST_CAP   = 64;   ///< a hostname or dotted quad
static const size_t TOKEN_CAP  = 65;   ///< the collector's INGEST_TOKEN
static const size_t USER_CAP   = 33;   ///< HTTP basic-auth user / password
static const size_t FW_CAP     = 24;   ///< "2026.09.1-dirty" and friends
static const size_t LMK_LEN    = 16;   ///< the ESP-NOW local master key

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// How the node reaches the collector. Read-only: the firmware reports it.
enum class Transport : uint8_t { Wifi = 0, EspNow = 1 };

/// Which chip the firmware is running on. Read-only, reported. It decides the
/// pin table (HwPins.h) every pin rule is checked against.
enum class Hw : uint8_t { Esp8266 = 0, Esp32c3 = 1 };

/// The sensors a node can drive — docs/NODE_CONFIG.md §1.1. `None` is only
/// ever the state of an unused slot; the codec never produces it for a
/// present entry and the validator refuses it.
enum class SensorType : uint8_t {
    None    = 0,
    Bmx280  = 1,   ///< BME280 / BMP280 on I2C
    Bme688  = 2,   ///< BME680 / BME688 on I2C (adds gas resistance)
    Ds18b20 = 3,   ///< 1-Wire probes, one entry per bus pin
    Bh1750  = 4,   ///< ambient light on I2C
    Sds011  = 5,   ///< particulate matter on a (soft) UART
    Pulse   = 6,   ///< reed / hall pulse counter: rain gauge or flow meter
};
static const uint8_t SENSOR_TYPE_COUNT = 7;   ///< including None

/// What a pulse counter measures, which decides its metric names and units.
enum class PulseMode : uint8_t { Rain = 0, Flow = 1 };

/// Wire names, the strings the JSON document uses. Index = enum value.
static inline const char* transportName(Transport t) {
    return t == Transport::EspNow ? "espnow" : "wifi";
}
static inline const char* hwName(Hw h) {
    return h == Hw::Esp32c3 ? "esp32c3" : "esp8266";
}
static inline const char* sensorTypeName(SensorType t) {
    switch (t) {
        case SensorType::Bmx280:  return "bmx280";
        case SensorType::Bme688:  return "bme688";
        case SensorType::Ds18b20: return "ds18b20";
        case SensorType::Bh1750:  return "bh1750";
        case SensorType::Sds011:  return "sds011";
        case SensorType::Pulse:   return "pulse";
        default:                  return "";
    }
}
static inline const char* pulseModeName(PulseMode m) {
    return m == PulseMode::Flow ? "flow" : "rain";
}

/// Parse the wire names back. Each returns false for anything it does not
/// know — including a different capitalisation, because the document is
/// machine-written and a near miss is a bug to surface, not to forgive.
static inline bool parseTransport(const char* s, Transport& out) {
    if (!s) return false;
    if (strcmp(s, "wifi") == 0)   { out = Transport::Wifi;   return true; }
    if (strcmp(s, "espnow") == 0) { out = Transport::EspNow; return true; }
    return false;
}
static inline bool parseHw(const char* s, Hw& out) {
    if (!s) return false;
    if (strcmp(s, "esp8266") == 0) { out = Hw::Esp8266; return true; }
    if (strcmp(s, "esp32c3") == 0) { out = Hw::Esp32c3; return true; }
    return false;
}
static inline bool parseSensorType(const char* s, SensorType& out) {
    if (!s || !*s) return false;
    for (uint8_t i = 1; i < SENSOR_TYPE_COUNT; i++) {
        if (strcmp(s, sensorTypeName((SensorType)i)) == 0) {
            out = (SensorType)i;
            return true;
        }
    }
    return false;
}
static inline bool parsePulseMode(const char* s, PulseMode& out) {
    if (!s) return false;
    if (strcmp(s, "rain") == 0) { out = PulseMode::Rain; return true; }
    if (strcmp(s, "flow") == 0) { out = PulseMode::Flow; return true; }
    return false;
}

/// Is this a sensor that cannot survive deep sleep between readings?
///
/// The SDS011 needs its fan spun up for ~30 s before a reading means anything,
/// and a pulse counter counts in an interrupt — both are simply not running
/// while the chip sleeps, so on a sleeping ESP-NOW node they would report
/// nonsense (a rain gauge that never tips) rather than fail. §1.2 refuses the
/// combination outright.
static inline bool sensorNeedsAwake(SensorType t) {
    return t == SensorType::Sds011 || t == SensorType::Pulse;
}

/// Does this sensor sit on the shared I2C bus (and so make i2c.sda/scl pins
/// that are in use)?
static inline bool sensorIsI2c(SensorType t) {
    return t == SensorType::Bmx280 || t == SensorType::Bme688 || t == SensorType::Bh1750;
}

// ---------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------

/// One entry of `sensors[]`. Flat rather than a union: every field is a byte
/// or four, eight entries cost ~200 bytes in total, and a flat struct can be
/// compared, copied and zeroed without knowing its type. The codec reads and
/// writes only the fields §1.1 lists for `type`; the rest sit at their
/// defaults and mean nothing.
struct SensorCfg {
    SensorType type = SensorType::None;

    /// I2C address: bmx280 / bme688 0x76 or 0x77 (0 = probe both, which is
    /// what the firmware has always done), bh1750 0x23 or 0x5C.
    uint8_t  addr = 0;

    /// ds18b20: the bus pin. pulse: the input pin.
    uint8_t  pin = 0;

    /// ds18b20: probes expected on this pin, 1..DS_MAX_COUNT. Each one is a
    /// metric, so this is what the budget counts.
    uint8_t  count = 1;

    /// ds18b20: base metric name. Probe 0 publishes it as is, probe N >= 1 as
    /// "<metric>_N". Defaults to probe_temp so a probe beside a BME280 does
    /// not collide with its "temperature" on the collector.
    char     metric[DS_METRIC_MAX + 1] = "probe_temp";

    /// sds011: the node's RX (the sensor's TXD) and TX pins.
    uint8_t  rx = 0;
    uint8_t  tx = 0;

    /// pulse: what it counts, how much one pulse is (mm of rain, or litres),
    /// and the dead time after an accepted edge that swallows contact bounce.
    PulseMode mode        = PulseMode::Rain;
    float     per_pulse   = 0.2794f;   // the common 0.011" tipping bucket
    uint32_t  debounce_us = 10000;
};

struct I2cCfg {
    uint8_t sda = 4;
    uint8_t scl = 5;
};

/// The network the WiFi node will try when its current one fails — §4.
/// Set by the collector during a handover; empty ssid = nothing to try.
struct NetNextCfg {
    char ssid[SSID_CAP] = "";
    char pass[PASS_CAP] = "";   ///< secret
};

/// WiFi node only.
///
/// Three secrets: pass, token, basic_pass (and next.pass). They are held here
/// in full because the node needs them and the collector must send them — but
/// the JSON encoder blanks them for every GET (docs/NODE_CONFIG.md §0.5).
struct NetCfg {
    char       ssid[SSID_CAP]       = "";
    char       pass[PASS_CAP]       = "";   ///< secret
    char       host[HOST_CAP]       = "";
    uint16_t   port                 = 80;
    char       token[TOKEN_CAP]     = "";   ///< secret
    char       basic_user[USER_CAP] = "";
    char       basic_pass[USER_CAP] = "";   ///< secret
    NetNextCfg next;
};

/// ESP-NOW node only: the radio link's tuning. Defaults are node_espnow's.
struct LinkCfg {
    /// How long the node listens for the ACK after a DATA frame.
    uint16_t ack_window_ms = 30;
    /// Unanswered wakes in a row before it sweeps the channels again.
    uint8_t  rescan_fails  = 3;
    /// Minimum seconds between two sweeps, so a collector that is simply off
    /// does not cost a full sweep every wake.
    uint32_t rescan_min_s  = 3600;
    /// §4.6: the collector's next network, tried when the stored SSID/BSSID
    /// is not on the air. No password — an ESP-NOW node never joins it.
    char     next_ssid[SSID_CAP] = "";
};

/// ESP-NOW node only: the battery divider (node_espnow/src/node_config.h
/// explains each value at length).
struct BattCfg {
    uint8_t pin     = 2;      ///< ADC1 pin; A0 = GPIO2 on the XIAO C3
    float   divider = 2.0f;   ///< cell volts / pin volts
    float   trim    = 1.0f;   ///< per-node calibration, 1.0 = none
};

/// The whole §1 document.
struct NodeConfig {
    /// The collector's revision this config came from; 0 = never synced.
    uint16_t   rev   = 0;
    /// Edited on the node's own page and not yet adopted by the collector.
    bool       local = false;

    // Read-only facts the node reports about itself.
    Transport  transport = Transport::Wifi;
    Hw         hw        = Hw::Esp8266;
    char       fw[FW_CAP] = "";

    char       name[NODE_NAME_MAX + 1] = "node";
    uint16_t   interval_s = 60;
    float      altitude_m = 0.0f;   ///< 0 = do not publish pressure_sea
    /// UI only: which silkscreen to draw. See HwPins.h for the ids.
    uint8_t    board = 0;
    /// ESP-NOW only: deep sleep between wakes. false = mains powered.
    bool       sleep = true;

    I2cCfg     i2c;
    uint8_t    sensor_count = 0;
    SensorCfg  sensors[MAX_SENSORS];

    NetCfg     net;    ///< meaningful when transport == Wifi
    LinkCfg    link;   ///< meaningful when transport == EspNow
    BattCfg    batt;   ///< meaningful when transport == EspNow

    /// ESP-NOW only, and only ever on the node's own page: the 16-byte key.
    /// Empty = use the one compiled in. docs/NODE_CONFIG.md §0.6 — it never
    /// travels over the radio, so the codec writes it only when asked to
    /// (NCJ_LMK) and never into a CFG / CFG_REPORT. The node stores it in NVS
    /// under its own key, not inside the document.
    char       lmk[LMK_LEN + 1] = "";
};

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

/// Bounded copy that always terminates. Returns false when `src` did not fit
/// — which callers that must not truncate (the JSON decoder) treat as an
/// error, and the rest ignore.
static inline bool copyStr(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return false;
    if (!src) { dst[0] = '\0'; return true; }
    const size_t n = strlen(src);
    const size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, k);
    dst[k] = '\0';
    return n < cap;
}

/// Append `s` to the NUL-terminated string in `dst`, cutting at `cap`.
///
/// This and strAppendUint() build every field path ("sensors[3].pin") and
/// metric name ("probe_temp_2") in src/nodecfg/ instead of snprintf, and not
/// for speed: GCC's -Wformat-truncation cannot see that a sensor index is
/// below 8, assumes ten digits, and warns on every such call — and the host
/// tests build with warnings on and are meant to stay silent.
static inline void strAppend(char* dst, size_t cap, const char* s) {
    if (!dst || cap == 0 || !s) return;
    size_t n = strnlen(dst, cap);
    while (*s && n + 1 < cap) dst[n++] = *s++;
    if (n < cap) dst[n] = '\0';
}

static inline void strAppendUint(char* dst, size_t cap, unsigned long v) {
    char tmp[12];
    size_t i = sizeof(tmp) - 1;
    tmp[i] = '\0';
    do {
        tmp[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    strAppend(dst, cap, tmp + i);
}

/// "sensors[<idx>].<key>" (or "sensors[<idx>]" when key is null/empty) into
/// `out` — the field path every error about a sensor entry uses.
static inline void sensorFieldPath(char* out, size_t cap, unsigned idx, const char* key) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    strAppend(out, cap, "sensors[");
    strAppendUint(out, cap, idx);
    strAppend(out, cap, "]");
    if (key && key[0]) {
        strAppend(out, cap, ".");
        strAppend(out, cap, key);
    }
}

/// A new sensor entry of `type` with the pins that suit `hw`.
///
/// The pin defaults are chosen so that the common one-extra-sensor node works
/// untouched: on the ESP8266 they are node/'s historical defaults (1-Wire on
/// D6, SDS011 on D5/D7, pulse on D2 — which does share GPIO4 with I2C SDA, a
/// clash node_config.h documents and the validator reports); on the C3 they
/// avoid the I2C pair (6/7), the battery ADC (2) and every strap.
static inline SensorCfg sensorDefaults(SensorType type, Hw hw) {
    SensorCfg s;
    s.type = type;
    const bool c3 = (hw == Hw::Esp32c3);
    switch (type) {
        case SensorType::Bmx280:
        case SensorType::Bme688:
            s.addr = 0;               // probe 0x76 then 0x77, as always
            break;
        case SensorType::Bh1750:
            s.addr = 0x23;            // ADDR pin low, the usual breakout
            break;
        case SensorType::Ds18b20:
            s.pin   = c3 ? 3 : 12;    // XIAO D1 / NodeMCU D6
            s.count = 1;
            break;
        case SensorType::Sds011:
            s.rx = c3 ? 5 : 14;       // XIAO D3 / NodeMCU D5
            s.tx = c3 ? 10 : 13;      // XIAO D10 / NodeMCU D7
            break;
        case SensorType::Pulse:
            s.pin         = 4;        // XIAO D2 / NodeMCU D2
            s.mode        = PulseMode::Rain;
            s.per_pulse   = 0.2794f;
            s.debounce_us = 10000;
            break;
        default:
            break;
    }
    return s;
}

/// The pulse defaults that depend on the mode: a rain gauge's reed switch
/// wants a 10 ms debounce; a hall flow sensor legitimately pulses hundreds of
/// times a second and any debounce would cap it, so it gets none.
static inline void pulseModeDefaults(SensorCfg& s, PulseMode m) {
    s.mode        = m;
    s.per_pulse   = (m == PulseMode::Flow) ? 0.00222f : 0.2794f;
    s.debounce_us = (m == PulseMode::Flow) ? 0 : 10000;
}

/// A config with nothing in it but defaults for this kind of node. The
/// sensor list is EMPTY: the firmware seeds it (from NODE_SENSOR_* on the
/// WiFi node, from the BME280 it has always had on the ESP-NOW node), because
/// only the firmware knows what it was built with.
static inline NodeConfig configDefaults(Transport t, Hw hw) {
    NodeConfig c;
    c.transport = t;
    c.hw        = hw;
    if (hw == Hw::Esp32c3) {
        c.i2c.sda = 6;   // XIAO D4
        c.i2c.scl = 7;   // XIAO D5
    } else {
        c.i2c.sda = 4;   // NodeMCU D2
        c.i2c.scl = 5;   // NodeMCU D1
    }
    // A WiFi node is mains powered by assumption (it cannot sleep through a
    // TCP connection); the flag means nothing there and says so honestly.
    c.sleep = (t == Transport::EspNow);
    return c;
}

/// Append a sensor entry. False when the list is full.
static inline bool addSensor(NodeConfig& c, const SensorCfg& s) {
    if (c.sensor_count >= MAX_SENSORS) return false;
    c.sensors[c.sensor_count++] = s;
    return true;
}

}  // namespace nodecfg
