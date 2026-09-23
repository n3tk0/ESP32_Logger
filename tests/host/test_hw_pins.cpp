// Host unit tests for src/nodecfg/HwPins.h — the pin tables of both node chips.
//
// Two jobs. The first is the same as test_node_pins.cpp's: the silkscreen
// resolves to the right GPIO, the flash bus is refused, and pins that are
// merely awkward are warned about rather than banned. The second is new: the
// ESP8266 table here and node/src/NodePins.h must agree pin for pin while both
// exist (the old portal still reads NodePins.h), so the two are compared
// directly. A pin that is FORBIDDEN in one and FREE in the other would be a
// node that one page lets you brick and the other does not.
#include "node/src/NodePins.h"
#include "src/nodecfg/HwPins.h"
#include "check.h"

HostSerial Serial;   // the Arduino shim's logging sink, referenced by nothing here

using namespace nodecfg;

// ---------------------------------------------------------------------------
static void test_esp8266_agrees_with_nodepins() {
    for (int g = -1; g <= 20; g++) {
        const NodePins::Risk old = NodePins::riskOf(g);
        const PinRisk now = pinRisk(Hw::Esp8266, g);
        const int want = old == NodePins::RISK_NEVER ? PIN_FORBIDDEN
                       : old == NodePins::RISK_CAUTION ? PIN_WARN : PIN_FREE;
        CHECK_EQ((int)now, want);
    }
    // Every label NodePins knows resolves to the same GPIO here, on the
    // NodeMCU (the board whose header prints all of them).
    for (uint8_t i = 0; i < NodePins::LABEL_COUNT; i++) {
        CHECK_EQ(resolvePin(Hw::Esp8266, 0, NodePins::LABELS[i].label),
                 (int)NodePins::LABELS[i].gpio);
    }
    // And the D-label printed back for a pin is the same one.
    for (int g = 0; g <= 16; g++) {
        const char* a = NodePins::dLabelFor(g);
        const char* b = pinLabel(Hw::Esp8266, 0, g);
        if (a[0]) CHECK_STREQ(b, a);
    }
    // Same board layouts, same order, so a diagram drawn from either matches.
    CHECK_EQ((int)hwInfo(Hw::Esp8266).boardCount, (int)NodePins::BOARD_COUNT);
    for (uint8_t b = 0; b < NodePins::BOARD_COUNT; b++) {
        CHECK_STREQ(ESP8266_BOARDS[b].name, NodePins::BOARDS[b].name);
        CHECK_STREQ(ESP8266_BOARDS[b].left, NodePins::BOARDS[b].left);
        CHECK_STREQ(ESP8266_BOARDS[b].right, NodePins::BOARDS[b].right);
    }
}

// ---------------------------------------------------------------------------
static void test_contract_pin_lists() {
    // docs/NODE_CONFIG.md §1.2, verbatim.
    for (int g = 0; g <= 16; g++) {
        const bool forbidden = (g >= 6 && g <= 11);
        const bool warned = (g == 0 || g == 2 || g == 15 || g == 1 || g == 3 || g == 16);
        CHECK_EQ((int)pinRisk(Hw::Esp8266, g),
                 forbidden ? (int)PIN_FORBIDDEN : warned ? (int)PIN_WARN : (int)PIN_FREE);
    }
    for (int g = 0; g <= 21; g++) {
        const bool forbidden = (g >= 12 && g <= 17);
        const bool warned = (g == 2 || g == 8 || g == 9 || g == 18 || g == 19 ||
                             g == 20 || g == 21);
        CHECK_EQ((int)pinRisk(Hw::Esp32c3, g),
                 forbidden ? (int)PIN_FORBIDDEN : warned ? (int)PIN_WARN : (int)PIN_FREE);
    }
    // Off the chip is forbidden, never free.
    CHECK_EQ((int)pinRisk(Hw::Esp8266, 17), (int)PIN_FORBIDDEN);
    CHECK_EQ((int)pinRisk(Hw::Esp32c3, 22), (int)PIN_FORBIDDEN);
    CHECK_EQ((int)pinRisk(Hw::Esp32c3, -1), (int)PIN_FORBIDDEN);
    CHECK_EQ((int)pinRisk(Hw::Esp8266, 255), (int)PIN_FORBIDDEN);
}

// ---------------------------------------------------------------------------
static void test_reasons_fit_and_exist() {
    const Hw hws[2] = { Hw::Esp8266, Hw::Esp32c3 };
    for (int h = 0; h < 2; h++) {
        const HwInfo& info = hwInfo(hws[h]);
        for (uint8_t i = 0; i < info.noteCount; i++) {
            // "GPIO21: <why>" has to fit a 48-byte CFG_ACK reason.
            CHECK(info.notes[i].why[0] != '\0');
            CHECK(strlen(info.notes[i].why) <= PIN_WHY_MAX);
            CHECK(info.notes[i].gpio <= info.maxGpio);
        }
        for (int g = 0; g <= (int)info.maxGpio; g++) {
            const bool free = pinRisk(hws[h], g) == PIN_FREE;
            CHECK_EQ(pinWhy(hws[h], g)[0] == '\0', free);
        }
    }
}

// ---------------------------------------------------------------------------
static void test_resolve_esp8266() {
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "D6"), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "d6"), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "  D6 "), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "GPIO12"), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "gpio12"), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "12"), 12);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "6"), 6);      // the raw number, which is the flash clock
    CHECK_EQ(resolvePin(Hw::Esp8266, 1, "D6"), 12);    // D1 mini: same mapping
    CHECK_EQ(resolvePin(Hw::Esp8266, 2, "D6"), 12);    // ESP-12: forgiven via the other boards
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "CLK"), 6);

    CHECK_EQ(resolvePin(Hw::Esp8266, 0, ""), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "   "), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, nullptr), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "17"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "D9"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "GPIO"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "12a"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "-1"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "1 2"), -1);
    CHECK_EQ(resolvePin(Hw::Esp8266, 0, "0012"), -1);  // four digits is not a GPIO
}

static void test_resolve_esp32c3() {
    // XIAO ESP32-C3: the D-numbers are NOT the GPIOs, which is the whole
    // reason to resolve them. D6/D7 are the UART, D8/D9 the straps.
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D0"), 2);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D4"), 6);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D5"), 7);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D6"), 21);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D7"), 20);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D10"), 10);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "d10"), 10);
    // SuperMini prints the GPIO number itself.
    CHECK_EQ(resolvePin(Hw::Esp32c3, 1, "20"), 20);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 1, "GPIO21"), 21);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 2, "13"), 13);    // exists; refused later as flash
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "22"), -1);
    CHECK_EQ(resolvePin(Hw::Esp32c3, 0, "D11"), -1);

    CHECK_STREQ(pinLabel(Hw::Esp32c3, 0, 21), "D6");
    CHECK_STREQ(pinLabel(Hw::Esp32c3, 0, 2), "D0");
    CHECK_STREQ(pinLabel(Hw::Esp32c3, 1, 7), "7");
    CHECK_STREQ(pinLabel(Hw::Esp32c3, 2, 7), "");      // "other": nothing printed
    CHECK_STREQ(pinLabel(Hw::Esp32c3, 0, 14), "");     // flash: never labelled
    CHECK_STREQ(pinLabel(Hw::Esp8266, 0, 6), "");      // CLK: never echoed
    CHECK_STREQ(pinLabel(Hw::Esp8266, 0, 12), "D6");
    CHECK_STREQ(pinLabel(Hw::Esp8266, 9, 12), "");     // no such board
}

// ---------------------------------------------------------------------------
static void test_capabilities() {
    CHECK(!pinHasInterrupt(Hw::Esp8266, 16));
    CHECK(pinHasInterrupt(Hw::Esp8266, 14));
    CHECK(pinHasInterrupt(Hw::Esp32c3, 16) == true);   // exists; flash rule is separate
    CHECK(!pinHasInterrupt(Hw::Esp32c3, 22));
    for (int g = 0; g <= 4; g++) CHECK(pinIsAdc(Hw::Esp32c3, g));
    CHECK(!pinIsAdc(Hw::Esp32c3, 5));                  // ADC2: the radio has it
    CHECK(!pinIsAdc(Hw::Esp8266, 0));                  // A0 is not a GPIO
}

// ---------------------------------------------------------------------------
static void test_tables_are_self_consistent() {
    const Hw hws[2] = { Hw::Esp8266, Hw::Esp32c3 };
    for (int h = 0; h < 2; h++) {
        const HwInfo& info = hwInfo(hws[h]);
        CHECK_EQ((int)info.boardCount, 3);
        for (uint8_t b = 0; b < info.boardCount; b++) {
            const BoardInfo& bi = info.boards[b];
            CHECK_EQ((int)bi.id, (int)b);           // ids are positions: the stored `board`
            CHECK(bi.name && bi.name[0]);
            for (uint8_t i = 0; i < bi.labelCount; i++) {
                CHECK(pinExists(hws[h], bi.labels[i].gpio));
                CHECK_EQ(resolvePin(hws[h], b, bi.labels[i].label), (int)bi.labels[i].gpio);
                // Labels are unique within a board (they are JSON keys in caps).
                for (uint8_t j = 0; j < i; j++)
                    CHECK(strcmp(bi.labels[i].label, bi.labels[j].label) != 0);
            }
        }
        // Notes are unique per GPIO.
        for (uint8_t i = 0; i < info.noteCount; i++)
            for (uint8_t j = 0; j < i; j++)
                CHECK(info.notes[i].gpio != info.notes[j].gpio);
    }
}

int main() {
    RUN(test_esp8266_agrees_with_nodepins);
    RUN(test_contract_pin_lists);
    RUN(test_reasons_fit_and_exist);
    RUN(test_resolve_esp8266);
    RUN(test_resolve_esp32c3);
    RUN(test_capabilities);
    RUN(test_tables_are_self_consistent);
    return SUMMARY();
}
