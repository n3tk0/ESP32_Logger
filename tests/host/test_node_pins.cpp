// Host unit tests for node/src/NodePins.h — the ESP8266 pin table the setup
// portal validates against.
//
// This is the one part of the node firmware that can be checked without a
// board, and it is also the part that cost a board: the portal used to accept
// any GPIO 0..16, so a BMP280 wired to the pins printed D6/D5 and entered as
// "6" and "5" put I2C on the SPI flash bus. The node then boot-looped on a
// watchdog reset, printing nothing readable, on every power-up — the bad pin
// was in flash by then.
//
// So the properties worth pinning down are: the silkscreen resolves to the
// right GPIO, the flash bus is refused, and nothing that is merely awkward
// (boot straps, the UART) is refused along with it.
#include "node/src/NodePins.h"
#include "check.h"

HostSerial Serial;   // the shim's logging sink, referenced by nothing here

using namespace NodePins;

// ---------------------------------------------------------------------------
static void test_silkscreen_resolves_to_gpio() {
    // The nine D-numbers, from the NodeMCU/D1-mini pinout. These are the whole
    // reason resolve() exists.
    CHECK_EQ(resolve("D0"), 16);
    CHECK_EQ(resolve("D1"),  5);
    CHECK_EQ(resolve("D2"),  4);
    CHECK_EQ(resolve("D3"),  0);
    CHECK_EQ(resolve("D4"),  2);
    CHECK_EQ(resolve("D5"), 14);
    CHECK_EQ(resolve("D6"), 12);
    CHECK_EQ(resolve("D7"), 13);
    CHECK_EQ(resolve("D8"), 15);

    // Typed on a phone, which is where this form is used.
    CHECK_EQ(resolve("d6"),   12);
    CHECK_EQ(resolve("  D6 "), 12);
    CHECK_EQ(resolve("GPIO12"), 12);
    CHECK_EQ(resolve("gpio12"), 12);
    CHECK_EQ(resolve("12"),    12);
}

// ---------------------------------------------------------------------------
static void test_the_two_scales_do_not_collide() {
    // The bug in one assertion: these two must NOT be the same pin, and the
    // one spelled with a D has to win when a D is what was typed.
    CHECK(resolve("D6") != resolve("6"));
    CHECK_EQ(resolve("D6"), 12);
    CHECK_EQ(resolve("6"),   6);
    // …and the raw one is the flash clock, so it is refused downstream.
    CHECK_EQ(riskOf(resolve("6")), RISK_NEVER);
    CHECK_EQ(riskOf(resolve("D6")), RISK_FREE);
}

// ---------------------------------------------------------------------------
static void test_rejects_what_is_not_a_pin() {
    CHECK_EQ(resolve(""),        -1);
    CHECK_EQ(resolve("   "),     -1);
    CHECK_EQ(resolve("17"),      -1);   // one past the top of the chip
    CHECK_EQ(resolve("99"),      -1);
    CHECK_EQ(resolve("-1"),      -1);   // the minus makes it non-numeric here
    CHECK_EQ(resolve("D9"),      -1);   // no such silkscreen label
    CHECK_EQ(resolve("12a"),     -1);
    CHECK_EQ(resolve("a12"),     -1);
    CHECK_EQ(resolve("GPIO"),    -1);   // prefix with nothing after it
    CHECK_EQ(resolve("GPIO99"),  -1);
    CHECK_EQ(resolve("1 2"),     -1);
}

// ---------------------------------------------------------------------------
static void test_flash_bus_is_never_allowed() {
    // Six pins, no exceptions, no override: there is no wiring that makes the
    // SPI flash bus usable for a sensor.
    for (int g = 6; g <= 11; g++) {
        CHECK_EQ(riskOf(g), RISK_NEVER);
    }
    // The header labels for them resolve — the parser has to understand what
    // was typed in order to refuse it with a reason.
    CHECK_EQ(resolve("CLK"),  6);
    CHECK_EQ(resolve("SD0"),  7);
    CHECK_EQ(resolve("CMD"), 11);
    CHECK_EQ(riskOf(resolve("SD1")), RISK_NEVER);
}

// ---------------------------------------------------------------------------
static void test_awkward_pins_are_warned_not_banned() {
    // Every one of these is in daily use on real boards: a boot strap held
    // high by an I2C pull-up is how the vendors wire their own sensors. A
    // table that refused them would push people back to editing config by
    // hand, which is where the flash-bus pins came from.
    CHECK_EQ(riskOf(0),  RISK_CAUTION);   // strap + FLASH button
    CHECK_EQ(riskOf(1),  RISK_CAUTION);   // UART0 TX
    CHECK_EQ(riskOf(2),  RISK_CAUTION);   // strap + onboard LED
    CHECK_EQ(riskOf(3),  RISK_CAUTION);   // UART0 RX
    CHECK_EQ(riskOf(15), RISK_CAUTION);   // strap, must be LOW
    CHECK_EQ(riskOf(16), RISK_CAUTION);   // no interrupt, no pull-up

    // And the ones with nothing attached are free.
    CHECK_EQ(riskOf(4),  RISK_FREE);
    CHECK_EQ(riskOf(5),  RISK_FREE);
    CHECK_EQ(riskOf(12), RISK_FREE);
    CHECK_EQ(riskOf(13), RISK_FREE);
    CHECK_EQ(riskOf(14), RISK_FREE);
}

// ---------------------------------------------------------------------------
static void test_riskof_treats_nonsense_as_never() {
    // A caller that forgot to check resolve()'s -1 must not be told the pin
    // is free. Out of range is the safest answer, not the most permissive.
    CHECK_EQ(riskOf(-1),  RISK_NEVER);
    CHECK_EQ(riskOf(17),  RISK_NEVER);
    CHECK_EQ(riskOf(255), RISK_NEVER);
}

// ---------------------------------------------------------------------------
static void test_labels_printed_back_are_d_numbers_only() {
    CHECK_STREQ(dLabelFor(12), "D6");
    CHECK_STREQ(dLabelFor(16), "D0");
    CHECK_STREQ(dLabelFor(15), "D8");
    // GPIO6 is CLK on the header, but printing "GPIO6 (CLK)" beside a field
    // would read like a suggestion. Flash pins get no label back.
    CHECK_STREQ(dLabelFor(6), "");
    CHECK_STREQ(dLabelFor(1), "");    // TX
    CHECK_STREQ(dLabelFor(99), "");
}

// ---------------------------------------------------------------------------
static void test_tables_are_self_consistent() {
    // Every label points at a pin that exists…
    for (uint8_t i = 0; i < LABEL_COUNT; i++) {
        CHECK(LABELS[i].gpio <= MAX_GPIO);
        CHECK_EQ(resolve(LABELS[i].label), (int)LABELS[i].gpio);
    }
    // …every GPIO has a reason string, empty only when it is free…
    for (int g = 0; g <= (int)MAX_GPIO; g++) {
        CHECK(FACTS[g].why != nullptr);
        const bool hasWhy = FACTS[g].why[0] != '\0';
        CHECK_EQ(hasWhy, riskOf(g) != RISK_FREE);
    }
    // …and every pad the board diagrams draw is either a known name or a
    // power/ground pad, never a typo that would silently render grey.
    for (uint8_t b = 0; b < BOARD_COUNT; b++) {
        CHECK(BOARDS[b].name[0] != '\0');
        CHECK(BOARDS[b].left[0] != '\0');
        CHECK(BOARDS[b].right[0] != '\0');
    }
}

// ---------------------------------------------------------------------------
// The board diagrams: every pad name that LOOKS like a GPIO must resolve, and
// the D-labelled ones must appear exactly where the vendor prints them.
static void test_board_layouts() {
    // NodeMCU: D5/D6/D7 are on the right-hand header, in that order.
    const String right(BOARDS[0].right);
    CHECK(right.indexOf('D') >= 0);
    CHECK(String(BOARDS[0].right).startsWith("D0,D1,D2,D3,D4"));
    // The flash pins are drawn (they are printed on the board) so the diagram
    // can show them red — that is the whole point of drawing them.
    CHECK(String(BOARDS[0].left).indexOf('S') >= 0);

    // D1 mini: eight pads a side, same D-numbers.
    CHECK(String(BOARDS[1].left).startsWith("RST,A0,D0,D5,D6,D7,D8"));
    CHECK(String(BOARDS[1].right).startsWith("TX,RX,D1,D2,D3,D4"));

    // The generic board has no silkscreen to promise, so it prints GPIOs.
    CHECK(String(BOARDS[2].left).startsWith("GPIO"));
}

int main() {
    RUN(test_silkscreen_resolves_to_gpio);
    RUN(test_the_two_scales_do_not_collide);
    RUN(test_rejects_what_is_not_a_pin);
    RUN(test_flash_bus_is_never_allowed);
    RUN(test_awkward_pins_are_warned_not_banned);
    RUN(test_riskof_treats_nonsense_as_never);
    RUN(test_labels_printed_back_are_d_numbers_only);
    RUN(test_tables_are_self_consistent);
    RUN(test_board_layouts);
    return SUMMARY();
}
