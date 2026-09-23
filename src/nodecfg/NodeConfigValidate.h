// ============================================================================
// src/nodecfg/NodeConfigValidate.h
//
// Is this config one a node can run? — docs/NODE_CONFIG.md §1.2, as code.
//
// ONE VALIDATOR, THREE CALLERS
// ----------------------------
// The node validates what it is sent (by its own page, or by the collector),
// and the collector validates what its UI posts before storing it as desired.
// Both call validate() below. That is the whole of §0.3: if the collector
// accepted a config, the node will too, and a config the node would refuse is
// refused at the collector with the same field and the same words — rather
// than accepted there, shown as "pending" for an hour, and then "rejected" by
// a node on a wall.
//
// WHAT COMES BACK
// ---------------
// The FIRST problem, as a machine-readable `field` path ("sensors[1].pin",
// "i2c.sda", "net.port", "sensors" for the list as a whole) and a short human
// `reason`. First, not all: the page fixes one field at a time anyway, and a
// deterministic single answer is what CFG_ACK can carry in 72 bytes. The
// sizes are CFG_ACK's, so a reason is never cut short on its way to the
// collector's UI — every reason below is written to fit, and the host test
// checks that by provoking each one.
//
// Plus warnings: pins that work but have a condition attached (boot straps,
// the console, USB). Accepted, shown by the page.
//
// No Arduino, no allocation, no JSON: a pure function over the struct, so the
// host tests can drive every rule one at a time.
// ============================================================================
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "NodeConfig.h"
#include "HwPins.h"
#include "MetricCatalog.h"
#include "../espnow/EspNowProto.h"

namespace nodecfg {

/// One problem: where, and why.
struct Issue {
    char field[EN_CFG_FIELD_LEN];
    char reason[EN_CFG_REASON_LEN];
};

static const uint8_t MAX_WARNINGS = 8;

struct Validation {
    bool    ok = true;
    Issue   error;                  ///< meaningful when !ok
    uint8_t warnCount = 0;
    Issue   warnings[MAX_WARNINGS]; ///< first MAX_WARNINGS; the rest are dropped
};

// Physical sanity ranges for the few floats. Wide on purpose: they exist to
// catch a typo (a divider of 0 makes every battery reading 0 V and the
// collector's battery estimate collapses), not to second-guess hardware.
static const float ALTITUDE_MIN_M = -500.0f;     // the Dead Sea shore is -430
static const float ALTITUDE_MAX_M = 9000.0f;
static const float DIVIDER_MIN    = 1.0f;        // a divider cannot amplify
static const float DIVIDER_MAX    = 20.0f;
static const float TRIM_MIN       = 0.5f;
static const float TRIM_MAX       = 1.5f;
static const uint32_t DEBOUNCE_MAX_US = 1000000;  // 1 s: past that it is not bounce

namespace detail {

static inline void setIssue(Issue& is, const char* field, const char* fmt, va_list ap) {
    copyStr(is.field, sizeof(is.field), field ? field : "");
    vsnprintf(is.reason, sizeof(is.reason), fmt, ap);
}

static inline bool fail(Validation& v, const char* field, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
static inline bool fail(Validation& v, const char* field, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    setIssue(v.error, field, fmt, ap);
    va_end(ap);
    v.ok = false;
    return false;
}

static inline void warn(Validation& v, const char* field, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
static inline void warn(Validation& v, const char* field, const char* fmt, ...) {
    if (v.warnCount >= MAX_WARNINGS) return;
    va_list ap;
    va_start(ap, fmt);
    setIssue(v.warnings[v.warnCount++], field, fmt, ap);
    va_end(ap);
}

static inline bool isFinite(float f) {
    // No <math.h>: -ffast-math on some toolchains folds isfinite() to true,
    // and a NaN divider is precisely what this has to catch.
    return f == f && f - f == 0.0f;
}

static inline bool nameChar(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

/// A pin assignment under test. The field path is kept as its parts, not
/// formatted, because the ESP8266 validates on a 4 KB stack and nineteen
/// formatted paths would be half a kilobyte of it.
struct PinUse {
    const char* key;      ///< "i2c.sda", "batt.pin", or the sensor field ("pin", "rx", …)
    int8_t      sensor;   ///< sensors[] index, or -1 for a top-level field
    uint8_t     gpio;
    uint8_t     role;     ///< what the pin must be able to do, see ROLE_*
};
static const uint8_t ROLE_ANY  = 0;
static const uint8_t ROLE_IRQ  = 1;   ///< needs an edge interrupt (pulse, soft RX)
static const uint8_t ROLE_ADC  = 2;   ///< needs an ADC1 channel (battery)

static inline void addUse(PinUse* u, uint8_t& n, const char* key, int sensor,
                          uint8_t gpio, uint8_t role) {
    u[n].key    = key;
    u[n].sensor = (int8_t)sensor;
    u[n].gpio   = gpio;
    u[n].role   = role;
    n++;
}

static inline void useField(const PinUse& u, char out[EN_CFG_FIELD_LEN]) {
    if (u.sensor < 0) copyStr(out, EN_CFG_FIELD_LEN, u.key);
    else              sensorFieldPath(out, EN_CFG_FIELD_LEN, (unsigned)u.sensor, u.key);
}

}  // namespace detail

/// Check `c` against every rule of docs/NODE_CONFIG.md §1.2 (plus the value
/// ranges the struct cannot express by itself). Returns out.ok; on false,
/// out.error names the first offending field. Warnings are filled either way
/// for the pins that were checked before the first error.
///
/// Rules, in the order they are checked — the order decides which error a
/// config with several gets, and the page relies on it being stable:
///
///   name        1..16 of [A-Za-z0-9_-]
///   interval_s  10..65535
///   altitude_m  finite, -500..9000
///   board       one of the chip's boards (HwPins.h)
///   sensors     at most 8 entries; each a known type with valid fields:
///                 bmx280/bme688 addr 0x76, 0x77 or 0 (probe both)
///                 bh1750 addr 0x23 or 0x5C
///                 ds18b20 count 1..8, metric 1..10 of [a-z0-9_] from a letter
///                 pulse per_pulse > 0, debounce_us <= 1 s
///               no sds011/pulse on a sleeping ESP-NOW node
///               one entry per type, except ds18b20
///               not bmx280 and bme688 together
///               at most 8 metrics in total (pressure_sea always counted)
///               no two metrics with the same name
///   pins        every pin in use exists on the chip, is not forbidden, and
///               is used once (I2C only when an I2C sensor is present, the
///               battery pin on ESP-NOW); pulse and ESP8266 sds011.rx need an
///               interrupt (not ESP8266 GPIO16); batt.pin must be ADC1
///   batt        (ESP-NOW) divider 1..20, trim 0.5..1.5
///   link        (ESP-NOW) ack_window_ms 5..1000, rescan_fails >= 1
///   net         (WiFi) ssid and host set, port >= 1, a passphrase (and
///               next.pass) empty or 8..64 characters
///   lmk         empty or exactly 16 characters
static inline bool validate(const NodeConfig& c, Validation& out) {
    using detail::fail;
    using detail::warn;
    out.ok = true;
    out.warnCount = 0;
    out.error.field[0] = '\0';
    out.error.reason[0] = '\0';

    const bool espnow = (c.transport == Transport::EspNow);
    char f[EN_CFG_FIELD_LEN];

    // --- identity ----------------------------------------------------------
    const size_t nameLen = strnlen(c.name, sizeof(c.name));
    if (nameLen == 0)
        return fail(out, "name", "must not be empty");
    if (nameLen > NODE_NAME_MAX)
        return fail(out, "name", "at most %u characters", (unsigned)NODE_NAME_MAX);
    for (size_t i = 0; i < nameLen; i++)
        if (!detail::nameChar(c.name[i]))
            return fail(out, "name", "only letters, digits, _ and -");

    if (c.interval_s < INTERVAL_MIN_S)
        return fail(out, "interval_s", "must be %u..%u seconds",
                    (unsigned)INTERVAL_MIN_S, (unsigned)INTERVAL_MAX_S);

    if (!detail::isFinite(c.altitude_m) || c.altitude_m < ALTITUDE_MIN_M ||
        c.altitude_m > ALTITUDE_MAX_M)
        return fail(out, "altitude_m", "must be -500..9000 m");

    if (!boardInfo(c.hw, c.board))
        return fail(out, "board", "not a board this chip can be");

    // --- sensors, one at a time ---------------------------------------------
    if (c.sensor_count > MAX_SENSORS)
        return fail(out, "sensors", "at most %u sensors", (unsigned)MAX_SENSORS);

    bool anyI2c = false;
    int  seenType[SENSOR_TYPE_COUNT];
    for (uint8_t t = 0; t < SENSOR_TYPE_COUNT; t++) seenType[t] = -1;

    for (uint8_t i = 0; i < c.sensor_count; i++) {
        const SensorCfg& s = c.sensors[i];
        const uint8_t t = (uint8_t)s.type;
        sensorFieldPath(f, sizeof(f), i, "type");
        if (t == 0 || t >= SENSOR_TYPE_COUNT)
            return fail(out, f, "unknown sensor type");

        switch (s.type) {
            case SensorType::Bmx280:
            case SensorType::Bme688:
                if (s.addr != 0 && s.addr != 0x76 && s.addr != 0x77) {
                    sensorFieldPath(f, sizeof(f), i, "addr");
                    return fail(out, f, "must be 0x76, 0x77, or 0 to probe both");
                }
                break;
            case SensorType::Bh1750:
                if (s.addr != 0x23 && s.addr != 0x5C) {
                    sensorFieldPath(f, sizeof(f), i, "addr");
                    return fail(out, f, "must be 0x23 or 0x5C");
                }
                break;
            case SensorType::Ds18b20: {
                if (s.count < 1 || s.count > DS_MAX_COUNT) {
                    sensorFieldPath(f, sizeof(f), i, "count");
                    return fail(out, f, "must be 1..%u probes", (unsigned)DS_MAX_COUNT);
                }
                sensorFieldPath(f, sizeof(f), i, "metric");
                const size_t ml = strnlen(s.metric, sizeof(s.metric));
                if (ml == 0)
                    return fail(out, f, "must not be empty");
                if (ml > DS_METRIC_MAX)
                    return fail(out, f, "at most %u characters", (unsigned)DS_METRIC_MAX);
                // Lower case and underscores, starting with a letter: the
                // shape of every metric name the collector already has, and
                // what MQTT topics and CSV headers downstream assume.
                if (!(s.metric[0] >= 'a' && s.metric[0] <= 'z'))
                    return fail(out, f, "must start with a lower-case letter");
                for (size_t k = 0; k < ml; k++) {
                    const char ch = s.metric[k];
                    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                        return fail(out, f, "only a-z, 0-9 and _");
                }
                break;
            }
            case SensorType::Pulse:
                if (!detail::isFinite(s.per_pulse) || s.per_pulse <= 0.0f) {
                    sensorFieldPath(f, sizeof(f), i, "per_pulse");
                    return fail(out, f, "must be greater than 0");
                }
                if (s.debounce_us > DEBOUNCE_MAX_US) {
                    sensorFieldPath(f, sizeof(f), i, "debounce_us");
                    return fail(out, f, "at most 1000000 us");
                }
                if (s.mode != PulseMode::Rain && s.mode != PulseMode::Flow) {
                    sensorFieldPath(f, sizeof(f), i, "mode");
                    return fail(out, f, "must be rain or flow");
                }
                break;
            default:
                break;
        }

        sensorFieldPath(f, sizeof(f), i, "type");
        if (espnow && c.sleep && sensorNeedsAwake(s.type))
            return fail(out, f, "%s needs power between readings: no sleep",
                        sensorTypeName(s.type));

        if (s.type != SensorType::Ds18b20 && seenType[t] >= 0)
            return fail(out, f, "only one %s per node", sensorTypeName(s.type));
        if ((s.type == SensorType::Bmx280 && seenType[(uint8_t)SensorType::Bme688] >= 0) ||
            (s.type == SensorType::Bme688 && seenType[(uint8_t)SensorType::Bmx280] >= 0))
            return fail(out, f, "bmx280 and bme688 publish the same metrics");
        if (seenType[t] < 0) seenType[t] = i;
        if (sensorIsI2c(s.type)) anyI2c = true;
    }

    // --- the metrics those sensors add up to ---------------------------------
    const uint16_t metrics = configMetricCount(c);
    if (metrics > MAX_METRICS)
        return fail(out, "sensors", "%u metrics; a node may publish at most %u",
                    (unsigned)metrics, (unsigned)MAX_METRICS);

    {
        MetricSlot slots[MAX_METRIC_SLOTS];
        const uint8_t n = listMetrics(c, slots, MAX_METRIC_SLOTS, false);
        for (uint8_t a = 0; a < n && a < MAX_METRIC_SLOTS; a++) {
            for (uint8_t b = 0; b < a; b++) {
                if (strcmp(slots[a].name, slots[b].name) != 0) continue;
                const SensorCfg& s = c.sensors[slots[a].sensor];
                sensorFieldPath(f, sizeof(f), slots[a].sensor,
                                s.type == SensorType::Ds18b20 ? "metric" : "type");
                return fail(out, f, "metric %s is already in use", slots[a].name);
            }
        }
    }

    // --- pins ---------------------------------------------------------------
    // Gathered first, then checked in one pass, so "used twice" can name the
    // field that had it first.
    detail::PinUse uses[2 + MAX_SENSORS * 2 + 1];
    uint8_t nu = 0;
    if (anyI2c) {
        detail::addUse(uses, nu, "i2c.sda", -1, c.i2c.sda, detail::ROLE_ANY);
        detail::addUse(uses, nu, "i2c.scl", -1, c.i2c.scl, detail::ROLE_ANY);
    }
    for (uint8_t i = 0; i < c.sensor_count; i++) {
        const SensorCfg& s = c.sensors[i];
        switch (s.type) {
            case SensorType::Ds18b20:
                detail::addUse(uses, nu, "pin", i, s.pin, detail::ROLE_ANY);
                break;
            case SensorType::Pulse:
                detail::addUse(uses, nu, "pin", i, s.pin, detail::ROLE_IRQ);
                break;
            case SensorType::Sds011:
                // SoftwareSerial RX on the ESP8266 runs on a pin-change
                // interrupt; the C3 has a spare hardware UART and does not care.
                detail::addUse(uses, nu, "rx", i, s.rx,
                               c.hw == Hw::Esp8266 ? detail::ROLE_IRQ : detail::ROLE_ANY);
                detail::addUse(uses, nu, "tx", i, s.tx, detail::ROLE_ANY);
                break;
            default:
                break;
        }
    }
    if (espnow) detail::addUse(uses, nu, "batt.pin", -1, c.batt.pin, detail::ROLE_ADC);

    for (uint8_t i = 0; i < nu; i++) {
        const detail::PinUse& u = uses[i];
        const int g = u.gpio;
        detail::useField(u, f);
        if (!pinExists(c.hw, g))
            return fail(out, f, "GPIO%d is not a pin on the %s", g, hwName(c.hw));
        if (pinRisk(c.hw, g) == PIN_FORBIDDEN)
            return fail(out, f, "GPIO%d is the SPI flash bus", g);
        for (uint8_t j = 0; j < i; j++) {
            if (uses[j].gpio != u.gpio) continue;
            char first[EN_CFG_FIELD_LEN];
            detail::useField(uses[j], first);
            return fail(out, f, "GPIO%d is already used by %s", g, first);
        }
        if (u.role == detail::ROLE_IRQ && !pinHasInterrupt(c.hw, g))
            return fail(out, f, "GPIO%d has no interrupt; pick another pin", g);
        if (u.role == detail::ROLE_ADC && !pinIsAdc(c.hw, g))
            return fail(out, f, "must be an ADC1 pin (GPIO0-4)");
        if (pinRisk(c.hw, g) == PIN_WARN)
            warn(out, f, "GPIO%d: %s", g, pinWhy(c.hw, g));
    }

    // --- transport-specific --------------------------------------------------
    if (espnow) {
        if (!detail::isFinite(c.batt.divider) || c.batt.divider < DIVIDER_MIN ||
            c.batt.divider > DIVIDER_MAX)
            return fail(out, "batt.divider", "must be 1..20");
        if (!detail::isFinite(c.batt.trim) || c.batt.trim < TRIM_MIN || c.batt.trim > TRIM_MAX)
            return fail(out, "batt.trim", "must be 0.5..1.5");
        if (c.link.ack_window_ms < 5 || c.link.ack_window_ms > 1000)
            return fail(out, "link.ack_window_ms", "must be 5..1000 ms");
        if (c.link.rescan_fails < 1)
            return fail(out, "link.rescan_fails", "must be at least 1");
    } else {
        if (c.net.ssid[0] == '\0')
            return fail(out, "net.ssid", "the WiFi network is required");
        if (c.net.host[0] == '\0')
            return fail(out, "net.host", "the collector address is required");
        if (c.net.port == 0)
            return fail(out, "net.port", "must be 1..65535");
        const size_t pl = strnlen(c.net.pass, sizeof(c.net.pass));
        if (pl > 0 && pl < 8)
            return fail(out, "net.pass", "a WPA2 passphrase is 8..63 characters");
        const size_t npl = strnlen(c.net.next.pass, sizeof(c.net.next.pass));
        if (npl > 0 && npl < 8)
            return fail(out, "net.next.pass", "a WPA2 passphrase is 8..63 characters");
    }

    const size_t lmkLen = strnlen(c.lmk, sizeof(c.lmk));
    if (lmkLen != 0 && lmkLen != LMK_LEN)
        return fail(out, "lmk", "must be exactly %u characters", (unsigned)LMK_LEN);

    return true;
}

}  // namespace nodecfg
