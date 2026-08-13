#include "I2CBus.h"
#include "../core/BoardProfiles.h"   // validateAttachPin
#include <soc/soc_caps.h>            // SOC_I2C_NUM

namespace {

struct BusState {
    TwoWire* wire       = nullptr;
    int      sda        = -1;
    int      scl        = -1;
    bool     configured = false;
};

BusState g_buses[I2CBus::MAX_BUSES];

// Maps a bus index to the core's TwoWire instance. Both objects always exist
// (Wire.cpp instantiates them unconditionally); whether the underlying
// controller exists is a separate question answered by hardwareBusCount().
TwoWire* wireFor(uint8_t bus) {
    switch (bus) {
        case 0:  return &Wire;
        case 1:  return &Wire1;
        default: return nullptr;
    }
}

}  // namespace

namespace I2CBus {

// ---------------------------------------------------------------------------
uint8_t hardwareBusCount() {
    // SOC_I2C_NUM is 1 on ESP32-C3, 2 on ESP32-S3 and classic ESP32. Clamped
    // to MAX_BUSES so a future part with more controllers cannot index past
    // g_buses[].
    constexpr uint8_t soc = (uint8_t)SOC_I2C_NUM;
    return (soc < MAX_BUSES) ? soc : MAX_BUSES;
}

// ---------------------------------------------------------------------------
TwoWire* acquire(uint8_t bus, int sda, int scl, const char* who) {
    const char* id = (who && *who) ? who : "i2c";

    if (bus >= MAX_BUSES) {
        Serial.printf("[I2C] %s: bus %u invalid (max %u)\n",
                      id, (unsigned)bus, (unsigned)(MAX_BUSES - 1));
        return nullptr;
    }
    if (bus >= hardwareBusCount()) {
        Serial.printf("[I2C] %s: bus %u not present on this chip "
                      "(%u controller%s) — use bus 0\n",
                      id, (unsigned)bus, (unsigned)hardwareBusCount(),
                      hardwareBusCount() == 1 ? "" : "s");
        return nullptr;
    }

    // Board-profile gate, same as any other pin assignment. Done before the
    // conflict check so an invalid pin is reported as such rather than as a
    // mismatch against whatever was configured first.
    if (!validateAttachPin(sda, id, "sda")) return nullptr;
    if (!validateAttachPin(scl, id, "scl")) return nullptr;

    BusState& b = g_buses[bus];

    if (b.configured) {
        if (b.sda != sda || b.scl != scl) {
            // Refusing is the point. Calling begin() again would succeed and
            // silently move the bus, killing every sensor already on it.
            Serial.printf("[I2C] %s: bus %u already on SDA%d/SCL%d — refusing "
                          "conflicting SDA%d/SCL%d\n",
                          id, (unsigned)bus, b.sda, b.scl, sda, scl);
            return nullptr;
        }
        return b.wire;   // normal case: another sensor joining the same bus
    }

    TwoWire* w = wireFor(bus);
    if (!w) return nullptr;

    if (!w->begin((int8_t)sda, (int8_t)scl)) {
        Serial.printf("[I2C] %s: bus %u begin() failed on SDA%d/SCL%d\n",
                      id, (unsigned)bus, sda, scl);
        return nullptr;
    }

    b.wire       = w;
    b.sda        = sda;
    b.scl        = scl;
    b.configured = true;
    Serial.printf("[I2C] bus %u up on SDA%d/SCL%d (first claim: %s)\n",
                  (unsigned)bus, sda, scl, id);
    return w;
}

// ---------------------------------------------------------------------------
TwoWire* get(uint8_t bus) {
    if (bus >= MAX_BUSES) return nullptr;
    return g_buses[bus].configured ? g_buses[bus].wire : nullptr;
}

bool isConfigured(uint8_t bus) {
    return (bus < MAX_BUSES) && g_buses[bus].configured;
}

int sdaOf(uint8_t bus) {
    return (bus < MAX_BUSES && g_buses[bus].configured) ? g_buses[bus].sda : -1;
}

int sclOf(uint8_t bus) {
    return (bus < MAX_BUSES && g_buses[bus].configured) ? g_buses[bus].scl : -1;
}

// ---------------------------------------------------------------------------
void resetAll() {
    for (uint8_t i = 0; i < MAX_BUSES; i++) {
        BusState& b = g_buses[i];
        if (b.configured && b.wire) {
            // Release the controller so the next begin() can move the pins.
            // Without end(), begin() on different pins leaves the old GPIO
            // matrix routing in place on some core versions.
            b.wire->end();
        }
        b.wire       = nullptr;
        b.sda        = -1;
        b.scl        = -1;
        b.configured = false;
    }
}

}  // namespace I2CBus
