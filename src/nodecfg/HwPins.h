// ============================================================================
// src/nodecfg/HwPins.h
//
// What each GPIO of each node chip is, what the boards print beside it, and
// which ones must never carry a sensor.
//
// GENERALISED FROM node/src/NodePins.h, NOT REPLACING IT
// ------------------------------------------------------
// NodePins.h is the ESP8266 table the WiFi node's current portal uses, and
// tools/check_node_portal.py reads its tables to rebuild that page. It stays
// exactly as it is until the old portal is gone. This header is the same
// knowledge for both node chips, in a form with no Arduino dependency (plain
// const char*, no String) so the validator, the caps encoder and the
// collector can share it; tests/host/test_hw_pins.cpp checks that the two
// ESP8266 tables agree pin for pin, so they cannot drift while both exist.
//
// WHY A PIN TABLE IS WORTH A HEADER
// ---------------------------------
// The bug in NodePins.h's header comment is the reason: a BMP280 wired to
// the pins printed D6/D5 was entered as 6 and 5, GPIO6 is the ESP8266's SPI
// flash clock, and the node boot-looped forever on a setting it had saved to
// flash. The ESP32-C3 has the same trap one bank up (GPIO12-17). So: forbidden
// pins are refused by the validator on every side, the silkscreen labels are
// what the page offers so nobody translates "D6" by hand, and the merely
// awkward pins (boot straps, USB, the console) are warned about, not banned —
// a boot strap held high by an I2C pull-up is how vendors wire their own
// breakouts, and refusing it would push people back to editing JSON.
// ============================================================================
#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "NodeConfig.h"

namespace nodecfg {

enum PinRisk : uint8_t {
    PIN_FREE      = 0,   ///< nothing else wants this pin
    PIN_WARN      = 1,   ///< usable, with a condition the user has to meet
    PIN_FORBIDDEN = 2,   ///< no wiring makes this work (or not on this chip)
};

/// A GPIO that is not simply free.
struct PinNote {
    uint8_t     gpio;
    PinRisk     risk;
    const char* why;     ///< short, plain ASCII: it goes into JSON and logs
};

/// A silkscreen label and the GPIO it is.
struct PinLabel {
    const char* label;
    uint8_t     gpio;
};

/// One board the page can draw. `left` / `right` are the header pads top to
/// bottom as printed (USB at the top), comma separated, including pads that
/// are not GPIOs (GND, 3V3, …); the caps encoder turns them into arrays.
/// Empty for "other", where there is no silkscreen to promise.
struct BoardInfo {
    uint8_t         id;
    const char*     name;
    const PinLabel* labels;
    uint8_t         labelCount;
    const char*     left;
    const char*     right;
};

struct HwInfo {
    Hw               hw;
    uint8_t          maxGpio;      ///< highest GPIO number the chip has
    const PinNote*   notes;
    uint8_t          noteCount;
    const BoardInfo* boards;
    uint8_t          boardCount;
};

// ---------------------------------------------------------------------------
// ESP8266
// ---------------------------------------------------------------------------
// GPIO 0..16. The D-numbers mean the same GPIOs on the NodeMCU and the D1
// mini — those boards differ in shape, not in mapping.

// At most PIN_WHY_MAX characters each: a warning reads "GPIO15: <why>" and has
// to fit the 48-byte reason a CFG_ACK carries (the host test checks).
static const size_t PIN_WHY_MAX = 39;

static const char WHY8266_FLASH[]   = "SPI flash bus, never usable";
static const char WHY8266_STRAP_H[] = "boot strap, must be high at reset";
static const char WHY8266_STRAP_L[] = "boot strap: no pull-up, low at reset";
static const char WHY8266_TX[]      = "UART0 TX, the serial console";
static const char WHY8266_RX[]      = "UART0 RX, the serial console";
static const char WHY8266_16[]      = "no interrupt and no internal pull-up";

static const PinNote ESP8266_NOTES[] = {
    {  0, PIN_WARN,      WHY8266_STRAP_H },
    {  1, PIN_WARN,      WHY8266_TX      },
    {  2, PIN_WARN,      WHY8266_STRAP_H },
    {  3, PIN_WARN,      WHY8266_RX      },
    {  6, PIN_FORBIDDEN, WHY8266_FLASH   },
    {  7, PIN_FORBIDDEN, WHY8266_FLASH   },
    {  8, PIN_FORBIDDEN, WHY8266_FLASH   },
    {  9, PIN_FORBIDDEN, WHY8266_FLASH   },
    { 10, PIN_FORBIDDEN, WHY8266_FLASH   },
    { 11, PIN_FORBIDDEN, WHY8266_FLASH   },
    { 15, PIN_WARN,      WHY8266_STRAP_L },
    { 16, PIN_WARN,      WHY8266_16      },
};

/// NodeMCU: the D-numbers, the UART pads, and the flash pads it prints on the
/// left header — drawn so the page can show them red, never offered.
static const PinLabel ESP8266_NODEMCU_LABELS[] = {
    { "D0", 16 }, { "D1",  5 }, { "D2",  4 }, { "D3",  0 }, { "D4",  2 },
    { "D5", 14 }, { "D6", 12 }, { "D7", 13 }, { "D8", 15 },
    { "RX",  3 }, { "TX",  1 },
    { "SD0", 7 }, { "SD1", 8 }, { "SD2", 9 }, { "SD3", 10 },
    { "CMD", 11 }, { "CLK", 6 },
};
static const PinLabel ESP8266_D1MINI_LABELS[] = {
    { "D0", 16 }, { "D1",  5 }, { "D2",  4 }, { "D3",  0 }, { "D4",  2 },
    { "D5", 14 }, { "D6", 12 }, { "D7", 13 }, { "D8", 15 },
    { "RX",  3 }, { "TX",  1 },
};

static const BoardInfo ESP8266_BOARDS[] = {
    { 0, "NodeMCU V2/V3",
      ESP8266_NODEMCU_LABELS,
      (uint8_t)(sizeof(ESP8266_NODEMCU_LABELS) / sizeof(ESP8266_NODEMCU_LABELS[0])),
      "A0,RSV,RSV,SD3,SD2,SD1,CMD,SD0,CLK,GND,3V3,EN,RST,GND,VIN",
      "D0,D1,D2,D3,D4,3V3,GND,D5,D6,D7,D8,RX,TX,GND,3V3" },
    { 1, "Wemos D1 mini",
      ESP8266_D1MINI_LABELS,
      (uint8_t)(sizeof(ESP8266_D1MINI_LABELS) / sizeof(ESP8266_D1MINI_LABELS[0])),
      "RST,A0,D0,D5,D6,D7,D8,3V3",
      "TX,RX,D1,D2,D3,D4,GND,5V" },
    { 2, "Bare ESP-12 / other", nullptr, 0,
      "GPIO16,GPIO14,GPIO12,GPIO13,GPIO15,GPIO2,GPIO0,GPIO4",
      "GPIO5,GPIO3,GPIO1,ADC,EN,RST,GND,3V3" },
};

// ---------------------------------------------------------------------------
// ESP32-C3
// ---------------------------------------------------------------------------
// GPIO 0..21. 12-17 are the in-package / SPI flash and are not broken out on
// either board — they are listed so a hand-typed "12" is refused with a
// reason instead of taking the flash away. 18/19 are the native USB the
// console and the upload both run over on these boards.

static const char WHYC3_FLASH[]  = "SPI flash bus, never usable";
static const char WHYC3_STRAP2[] = "boot strap, high at reset (XIAO A0)";
static const char WHYC3_STRAP8[] = "boot strap, must be high at reset";
static const char WHYC3_STRAP9[] = "BOOT button strap, high at reset";
static const char WHYC3_USB_DM[] = "USB D-, the console and upload port";
static const char WHYC3_USB_DP[] = "USB D+, the console and upload port";
static const char WHYC3_RX[]     = "UART0 RX, the boot console";
static const char WHYC3_TX[]     = "UART0 TX, the boot console";

static const PinNote ESP32C3_NOTES[] = {
    {  2, PIN_WARN,      WHYC3_STRAP2 },
    {  8, PIN_WARN,      WHYC3_STRAP8 },
    {  9, PIN_WARN,      WHYC3_STRAP9 },
    { 12, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 13, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 14, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 15, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 16, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 17, PIN_FORBIDDEN, WHYC3_FLASH  },
    { 18, PIN_WARN,      WHYC3_USB_DM },
    { 19, PIN_WARN,      WHYC3_USB_DP },
    { 20, PIN_WARN,      WHYC3_RX     },
    { 21, PIN_WARN,      WHYC3_TX     },
};

/// Seeed XIAO ESP32-C3. D6/D7 are the UART pins (21/20), D8/D9 the straps.
static const PinLabel ESP32C3_XIAO_LABELS[] = {
    { "D0",  2 }, { "D1",  3 }, { "D2",  4 }, { "D3",  5 }, { "D4",  6 },
    { "D5",  7 }, { "D6", 21 }, { "D7", 20 }, { "D8",  8 }, { "D9",  9 },
    { "D10", 10 },
};
/// The "ESP32-C3 SuperMini": the silkscreen is the bare GPIO number.
static const PinLabel ESP32C3_SUPERMINI_LABELS[] = {
    { "0", 0 }, { "1", 1 }, { "2", 2 }, { "3", 3 }, { "4", 4 }, { "5", 5 },
    { "6", 6 }, { "7", 7 }, { "8", 8 }, { "9", 9 }, { "10", 10 },
    { "20", 20 }, { "21", 21 },
};

static const BoardInfo ESP32C3_BOARDS[] = {
    { 0, "Seeed XIAO ESP32-C3",
      ESP32C3_XIAO_LABELS,
      (uint8_t)(sizeof(ESP32C3_XIAO_LABELS) / sizeof(ESP32C3_XIAO_LABELS[0])),
      "D0,D1,D2,D3,D4,D5,D6",
      "5V,GND,3V3,D10,D9,D8,D7" },
    { 1, "ESP32-C3 SuperMini",
      ESP32C3_SUPERMINI_LABELS,
      (uint8_t)(sizeof(ESP32C3_SUPERMINI_LABELS) / sizeof(ESP32C3_SUPERMINI_LABELS[0])),
      "5V,GND,3V3,4,3,2,1,0",
      "5,6,7,8,9,10,20,21" },
    { 2, "Other ESP32-C3", nullptr, 0, "", "" },
};

static const HwInfo HW_ESP8266 = {
    Hw::Esp8266, 16,
    ESP8266_NOTES, (uint8_t)(sizeof(ESP8266_NOTES) / sizeof(ESP8266_NOTES[0])),
    ESP8266_BOARDS, (uint8_t)(sizeof(ESP8266_BOARDS) / sizeof(ESP8266_BOARDS[0])),
};
static const HwInfo HW_ESP32C3 = {
    Hw::Esp32c3, 21,
    ESP32C3_NOTES, (uint8_t)(sizeof(ESP32C3_NOTES) / sizeof(ESP32C3_NOTES[0])),
    ESP32C3_BOARDS, (uint8_t)(sizeof(ESP32C3_BOARDS) / sizeof(ESP32C3_BOARDS[0])),
};

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

static inline const HwInfo& hwInfo(Hw hw) {
    return hw == Hw::Esp32c3 ? HW_ESP32C3 : HW_ESP8266;
}

static inline bool pinExists(Hw hw, int gpio) {
    return gpio >= 0 && gpio <= (int)hwInfo(hw).maxGpio;
}

static inline const PinNote* pinNote(Hw hw, int gpio) {
    const HwInfo& h = hwInfo(hw);
    for (uint8_t i = 0; i < h.noteCount; i++)
        if ((int)h.notes[i].gpio == gpio) return &h.notes[i];
    return nullptr;
}

/// The risk class of a GPIO. Anything the chip does not have is FORBIDDEN, so
/// a caller that skipped resolvePin()'s -1 cannot treat it as free.
static inline PinRisk pinRisk(Hw hw, int gpio) {
    if (!pinExists(hw, gpio)) return PIN_FORBIDDEN;
    const PinNote* n = pinNote(hw, gpio);
    return n ? n->risk : PIN_FREE;
}

/// Why a pin is not free; "" when it is, "not a GPIO on this chip" off-chip.
static inline const char* pinWhy(Hw hw, int gpio) {
    if (!pinExists(hw, gpio)) return "not a GPIO on this chip";
    const PinNote* n = pinNote(hw, gpio);
    return n ? n->why : "";
}

/// Can this pin take an edge interrupt? Only the ESP8266's GPIO16 cannot —
/// it lives in the RTC block — which is what rules it out for a pulse
/// counter and for SoftwareSerial RX.
static inline bool pinHasInterrupt(Hw hw, int gpio) {
    if (!pinExists(hw, gpio)) return false;
    return !(hw == Hw::Esp8266 && gpio == 16);
}

/// Is this pin an ADC input usable while WiFi/ESP-NOW is on? On the C3 that
/// is ADC1, GPIO0-4 (ADC2 on GPIO5 is taken by the radio). The ESP8266's
/// only ADC is the dedicated A0 pad, which is not a GPIO at all.
static inline bool pinIsAdc(Hw hw, int gpio) {
    return hw == Hw::Esp32c3 && gpio >= 0 && gpio <= 4;
}

static inline const BoardInfo* boardInfo(Hw hw, uint8_t board) {
    const HwInfo& h = hwInfo(hw);
    for (uint8_t i = 0; i < h.boardCount; i++)
        if (h.boards[i].id == board) return &h.boards[i];
    return nullptr;
}

/// The silkscreen label for a GPIO on `board`, or "" when the board prints
/// none. Flash pins get "" back even where the header prints them (CLK, SD0,
/// …): a label printed beside a field reads like an endorsement.
static inline const char* pinLabel(Hw hw, uint8_t board, int gpio) {
    if (pinRisk(hw, gpio) == PIN_FORBIDDEN) return "";
    const BoardInfo* b = boardInfo(hw, board);
    if (!b) return "";
    for (uint8_t i = 0; i < b->labelCount; i++)
        if ((int)b->labels[i].gpio == gpio) return b->labels[i].label;
    return "";
}

/// Case-insensitive equality of a trimmed span with a label.
static inline bool pinSpanEq(const char* s, size_t n, const char* label) {
    size_t i = 0;
    for (; i < n && label[i]; i++)
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)label[i])) return false;
    return i == n && label[i] == '\0';
}

/// Resolve what someone typed into a GPIO number: "12", "GPIO12", "gpio12",
/// and the board's own label ("D6", "d6") are all accepted. The board given
/// is tried first, then every other board of the chip (the labels never
/// disagree within a chip, so this is only forgiving, never ambiguous).
/// Returns -1 for anything this chip does not have — including an empty
/// field, which is not a pin either.
static inline int resolvePin(Hw hw, uint8_t board, const char* raw) {
    if (!raw) return -1;
    while (*raw == ' ' || *raw == '\t') raw++;
    size_t n = strlen(raw);
    while (n && (raw[n - 1] == ' ' || raw[n - 1] == '\t')) n--;
    if (n == 0) return -1;

    const HwInfo& h = hwInfo(hw);
    const BoardInfo* first = boardInfo(hw, board);
    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t bi = 0; bi < h.boardCount; bi++) {
            const BoardInfo* b = &h.boards[bi];
            if ((pass == 0) != (b == first)) continue;
            for (uint8_t i = 0; i < b->labelCount; i++)
                if (pinSpanEq(raw, n, b->labels[i].label)) return b->labels[i].gpio;
        }
    }

    const char* d = raw;
    size_t dn = n;
    if (dn > 4 && pinSpanEq(raw, 4, "GPIO")) { d += 4; dn -= 4; }
    if (dn == 0 || dn > 3) return -1;
    int v = 0;
    for (size_t i = 0; i < dn; i++) {
        if (d[i] < '0' || d[i] > '9') return -1;
        v = v * 10 + (d[i] - '0');
    }
    return pinExists(hw, v) ? v : -1;
}

}  // namespace nodecfg
