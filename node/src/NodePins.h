// ============================================================================
// node/src/NodePins.h — what the ESP8266's pins are, and what each one costs
//
// WHY THIS IS ITS OWN HEADER
// --------------------------
// Two readers need the same answer and must never disagree: the setup portal,
// which refuses a save and colours the board diagram, and tests/host, which
// compiles this file with a desktop g++ and checks the answers. Nothing here
// touches hardware or the network — it is a lookup table and a parser — so it
// is the part of the node firmware that CAN be tested without a board, and the
// part where being wrong is most expensive.
//
// THE BUG THAT PUT IT HERE
// ------------------------
// The portal accepted any GPIO from 0 to 16 and wrote it to flash. Someone
// wiring a BMP280 to the pins printed D6/D5 typed 6 and 5, which are not those
// pins: D6 is GPIO12, and GPIO6 is the SPI flash clock. Wire.begin() took the
// flash bus away from the running program, and the node came back as
//
//     wdt reset
//     load 0x4010f000, len 3424, room 16
//     ~ld
//     <garbage, forever>
//
// on every boot, because the setting survived the reset. Hence two rules:
// GPIO6-11 are refused outright, and the silkscreen labels are accepted as
// input so "D6" never has to be translated by hand at all.
// ============================================================================
#pragma once

#include <Arduino.h>   // String; tests/host/shims supplies its own

// The host shim has no flash-string qualifiers. They cost nothing there.
#ifndef PROGMEM
#  define PROGMEM
#endif

namespace NodePins {

enum Risk : uint8_t {
    RISK_FREE    = 0,   ///< nothing else wants this pin
    RISK_CAUTION = 1,   ///< usable, with a condition the user has to meet
    RISK_NEVER   = 2,   ///< no wiring makes this work
};

// Separate constants rather than inline literals: seven reasons cover
// seventeen pins, and the duplication would be the thing that rots.
static const char WHY_FLASH[]   PROGMEM = "SPI flash bus \xE2\x80\x94 the chip cannot run with this pin repurposed";
static const char WHY_STRAP_H[] PROGMEM = "boot strap: must be HIGH at reset (an I2C pull-up satisfies it)";
static const char WHY_STRAP_L[] PROGMEM = "boot strap: must be LOW at reset \xE2\x80\x94 a pull-up here stops the board booting";
static const char WHY_UART_TX[] PROGMEM = "UART0 TX \xE2\x80\x94 the serial log comes out here";
static const char WHY_UART_RX[] PROGMEM = "UART0 RX \xE2\x80\x94 the serial console";
static const char WHY_WAKE[]    PROGMEM = "no interrupt and no internal pull-up (deep-sleep wake pin)";
static const char WHY_NONE[]    PROGMEM = "";

struct Facts {
    Risk        risk;
    const char* why;   ///< PROGMEM on the device; read it with FPSTR()
};

/// Indexed by GPIO number. The ESP8266 has 0..16 and nothing above it.
static const Facts FACTS[17] = {
    /* 0  */ { RISK_CAUTION, WHY_STRAP_H },
    /* 1  */ { RISK_CAUTION, WHY_UART_TX },
    /* 2  */ { RISK_CAUTION, WHY_STRAP_H },
    /* 3  */ { RISK_CAUTION, WHY_UART_RX },
    /* 4  */ { RISK_FREE,    WHY_NONE    },
    /* 5  */ { RISK_FREE,    WHY_NONE    },
    /* 6  */ { RISK_NEVER,   WHY_FLASH   },
    /* 7  */ { RISK_NEVER,   WHY_FLASH   },
    /* 8  */ { RISK_NEVER,   WHY_FLASH   },
    /* 9  */ { RISK_NEVER,   WHY_FLASH   },
    /* 10 */ { RISK_NEVER,   WHY_FLASH   },
    /* 11 */ { RISK_NEVER,   WHY_FLASH   },
    /* 12 */ { RISK_FREE,    WHY_NONE    },
    /* 13 */ { RISK_FREE,    WHY_NONE    },
    /* 14 */ { RISK_FREE,    WHY_NONE    },
    /* 15 */ { RISK_CAUTION, WHY_STRAP_L },
    /* 16 */ { RISK_CAUTION, WHY_WAKE    },
};

static const uint8_t MAX_GPIO = 16;

/// The silkscreen. D0-D8 mean the same GPIOs on the NodeMCU and the D1 mini —
/// those boards differ in shape, not in mapping — so one table serves both.
struct Label { const char* label; uint8_t gpio; };
static const Label LABELS[] = {
    { "D0", 16 }, { "D1",  5 }, { "D2",  4 }, { "D3",  0 }, { "D4",  2 },
    { "D5", 14 }, { "D6", 12 }, { "D7", 13 }, { "D8", 15 },
    // Not D-pins, but they are printed on the header and people type them.
    { "RX",   3 }, { "TX",   1 },
    { "SD0",  7 }, { "SD1",  8 }, { "SD2",  9 }, { "SD3", 10 },
    { "CMD", 11 }, { "CLK",  6 },
};
static const uint8_t LABEL_COUNT = sizeof(LABELS) / sizeof(LABELS[0]);

/// Is this label one of the D-numbers — the ones worth printing back at the
/// user? "GPIO6 (CLK)" would read like an endorsement of the flash clock, so
/// only D0-D8 are ever echoed.
///
/// Asked of the label itself rather than of its POSITION in the table. The
/// count used to be a hand-maintained 9, which made the answer depend on
/// LABELS[] staying sorted with the D-numbers first: inserting RX before D8
/// would have silently stopped GPIO15 having a name, in a log line whose
/// whole purpose is to print one.
static inline bool isDLabel(const char* label) {
    return label[0] == 'D' && label[1] >= '0' && label[1] <= '9'
        && label[2] == '\0';
}

/// Header layout, top to bottom, as the pads are printed on the board. Used
/// only to draw the diagram; names that are not GPIOs (GND, 3V3, RST…) are
/// rendered as plain pads.
struct BoardDef {
    const char* name;
    const char* left;
    const char* right;
};
static const BoardDef BOARDS[] = {
    { "NodeMCU V2/V3",
      "A0,RSV,RSV,SD3,SD2,SD1,CMD,SD0,CLK,GND,3V3,EN,RST,GND,VIN",
      "D0,D1,D2,D3,D4,3V3,GND,D5,D6,D7,D8,RX,TX,GND,3V3" },
    { "Wemos D1 mini",
      "RST,A0,D0,D5,D6,D7,D8,3V3",
      "TX,RX,D1,D2,D3,D4,GND,5V" },
    { "Bare ESP-12 / other",
      "GPIO16,GPIO14,GPIO12,GPIO13,GPIO15,GPIO2,GPIO0,GPIO4",
      "GPIO5,GPIO3,GPIO1,ADC,EN,RST,GND,3V3" },
};
static const uint8_t BOARD_COUNT = sizeof(BOARDS) / sizeof(BOARDS[0]);

/// Resolve what someone typed into a GPIO number: "12", "D6", "d6" and
/// "GPIO12" are all the same pin. Returns -1 for anything this chip does not
/// have — including an empty field, which is not a pin either.
static inline int resolve(const String& raw) {
    String v = raw;
    v.trim();
    if (v.length() == 0) return -1;
    v.toUpperCase();

    for (uint8_t i = 0; i < LABEL_COUNT; i++) {
        if (v == LABELS[i].label) return LABELS[i].gpio;
    }
    if (v.startsWith("GPIO")) v = v.substring(4);
    if (v.length() == 0) return -1;

    for (size_t i = 0; i < v.length(); i++) {
        const char c = v[(int)i];
        if (c < '0' || c > '9') return -1;
    }
    const long n = v.toInt();
    return (n >= 0 && n <= MAX_GPIO) ? (int)n : -1;
}

/// The risk class of a GPIO. Anything off the chip counts as RISK_NEVER, so a
/// caller that skipped resolve()'s -1 cannot accidentally treat it as free.
static inline Risk riskOf(int gpio) {
    if (gpio < 0 || gpio > (int)MAX_GPIO) return RISK_NEVER;
    return FACTS[gpio].risk;
}

/// The D-label for a GPIO, or "" when the boards do not print one.
static inline const char* dLabelFor(int gpio) {
    for (uint8_t i = 0; i < LABEL_COUNT; i++) {
        if ((int)LABELS[i].gpio == gpio && isDLabel(LABELS[i].label)) {
            return LABELS[i].label;
        }
    }
    return "";
}

}  // namespace NodePins
