// Host unit tests for src/web/KindleSlots.h
//
// The slot list is what the dashboard draws, so the things worth checking are
// the ones that decide whether a reader sees a sensible page or a broken one:
//
//   • the defaults reproduce the dashboard as it was before slots existed —
//     an upgrade must not rearrange a page somebody has been reading for
//     months;
//   • a slot with no reading leaves NO gap, because that is the whole point of
//     the BMP280 case: no humidity measured, no humidity row;
//   • packing never overflows a row and never reorders the list;
//   • anything arriving from a file is clamped before a renderer sees it.
#include <stdint.h>
#include <string.h>

#include "src/web/KindleSlots.h"
#include "check.h"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
static void test_defaults_reproduce_the_old_dashboard() {
    KindleSlotList l;
    kdSlotsDefault(l, "balcony", "livingroom");

    CHECK_EQ(l.count, 6);

    // Same six readings, in the same order, with the same emphasis.
    CHECK_STREQ(l.slot[0].sensorId, "balcony");
    CHECK_STREQ(l.slot[0].metric,   "temperature");
    CHECK_EQ((int)l.slot[0].size, (int)KSLOT_HERO);
    CHECK((l.slot[0].flags & KSLOTF_BOLD) != 0);

    CHECK_STREQ(l.slot[1].metric, "humidity");
    CHECK_STREQ(l.slot[2].metric, "pressure");
    CHECK((l.slot[2].flags & KSLOTF_TREND) != 0);   // pressure carries the arrow

    CHECK_STREQ(l.slot[3].sensorId, "livingroom");
    CHECK_STREQ(l.slot[3].metric,   "temperature");
    CHECK_STREQ(l.slot[4].metric,   "humidity");
    CHECK_STREQ(l.slot[5].metric,   "aqi");

    // An empty sensor id must not produce a slot keyed on "" — every reading
    // would miss and the page would be blank with no clue why.
    KindleSlotList d;
    kdSlotsDefault(d, "", nullptr);
    CHECK_EQ(d.count, 6);
    CHECK(d.slot[0].sensorId[0] != '\0');
    CHECK(d.slot[3].sensorId[0] != '\0');
}

// ---------------------------------------------------------------------------
// Labels and decimals
// ---------------------------------------------------------------------------
static void test_labels_come_from_the_table_or_the_slot() {
    KindleSlotList l;
    l.clear();
    l.add("s", "pm25", nullptr, KSLOT_SMALL);
    l.add("s", "temperature", "Спалня", KSLOT_MEDIUM);
    l.add("s", "unlisted_metric", nullptr, KSLOT_SMALL);

    CHECK_STREQ(kdSlotLabel(l.slot[0]), "PM2.5");        // from the table
    CHECK_STREQ(kdSlotLabel(l.slot[1]), "Спалня");       // the slot's own wins
    CHECK_STREQ(kdSlotLabel(l.slot[2]), "unlisted_metric");  // honest fallback
}

static void test_decimals_come_from_the_metric_unless_set() {
    KindleSlotList l;
    l.clear();
    l.add("s", "temperature", nullptr, KSLOT_HERO);       // 1 by convention
    l.add("s", "humidity",    nullptr, KSLOT_SMALL);      // 0
    l.add("s", "battery_voltage", nullptr, KSLOT_SMALL);  // 2
    l.add("s", "temperature", nullptr, KSLOT_SMALL, 0);   // overridden

    CHECK_EQ((int)kdSlotDecimals(l.slot[0]), 1);
    CHECK_EQ((int)kdSlotDecimals(l.slot[1]), 0);
    CHECK_EQ((int)kdSlotDecimals(l.slot[2]), 2);
    CHECK_EQ((int)kdSlotDecimals(l.slot[3]), 0);

    // A nonsense override is clamped rather than passed to a printf as %.*f
    // with a width nobody budgeted a buffer for.
    KindleSlot wild{};
    strcpy(wild.metric, "temperature");
    wild.decimals = 99;
    CHECK_EQ((int)kdSlotDecimals(wild), 3);
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------
static void test_a_row_never_overflows() {
    KindleSlotList l;
    l.clear();
    // 4 + 4 + 4 = 12 exactly, then one more that cannot fit.
    for (int i = 0; i < 4; i++) l.add("s", "pm25", nullptr, KSLOT_MEDIUM);

    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP);
    CHECK_EQ(n, 4);

    // Sum the widths per row; none may exceed twelve.
    int used[8] = {0};
    for (int i = 0; i < n; i++) {
        CHECK(p[i].row < 8);
        CHECK_EQ((int)p[i].col, used[p[i].row]);   // laid left to right, no holes
        used[p[i].row] += p[i].units;
    }
    for (int r = 0; r < 8; r++) CHECK(used[r] <= KSLOT_ROW_UNITS);

    CHECK_EQ((int)p[3].row, 1);                    // the fourth wrapped
    CHECK_EQ((int)p[3].col, 0);
}

// A HERO is the glance value. Beside a small slot it stops being one, so it
// takes its own row even when there is room next to it.
static void test_a_hero_owns_its_row() {
    KindleSlotList l;
    l.clear();
    l.add("s", "pm25",        nullptr, KSLOT_SMALL);   // 3 units, row 0
    l.add("s", "temperature", nullptr, KSLOT_HERO);    // must not join row 0
    l.add("s", "humidity",    nullptr, KSLOT_SMALL);   // must not join the hero

    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP);
    CHECK_EQ(n, 3);
    CHECK_EQ((int)p[0].row, 0);
    CHECK_EQ((int)p[1].row, 1);
    CHECK_EQ((int)p[1].col, 0);
    CHECK_EQ((int)p[2].row, 2);
}

// The case the whole feature exists for.
static void test_an_absent_reading_leaves_no_gap() {
    KindleSlotList l;
    l.clear();
    l.add("bmp280", "temperature", nullptr, KSLOT_MEDIUM);
    l.add("bmp280", "humidity",    nullptr, KSLOT_MEDIUM);   // a BMP280 has none
    l.add("bmp280", "pressure",    nullptr, KSLOT_MEDIUM);

    const bool visible[3] = { true, false, true };
    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, visible, p, KindleSlotList::CAP);

    CHECK_EQ(n, 2);
    CHECK_EQ(p[0].index, 0);
    CHECK_EQ(p[1].index, 2);
    // Pressure moves up beside temperature — it does not sit in the third
    // position with a hole where the humidity was.
    CHECK_EQ((int)p[1].row, 0);
    CHECK_EQ((int)p[1].col, 4);
    CHECK_EQ(kdSlotsRowCount(p, n), 1);
}

// Packing must not reorder. A best-fit packer would fill the gap left by a
// quiet sensor with whatever happened to fit, so the reader's page would
// rearrange itself every time a node dropped out.
static void test_packing_preserves_order() {
    KindleSlotList l;
    l.clear();
    l.add("s", "temperature", nullptr, KSLOT_LARGE);   // 6
    l.add("s", "pm25",        nullptr, KSLOT_SMALL);   // 3  -> row 0, col 6
    l.add("s", "pressure",    nullptr, KSLOT_LARGE);   // 6  -> wraps to row 1
    l.add("s", "humidity",    nullptr, KSLOT_SMALL);   // 3  -> would have fitted row 0

    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP);
    CHECK_EQ(n, 4);
    for (int i = 0; i < n; i++) CHECK_EQ(p[i].index, i);   // never resequenced
    CHECK_EQ((int)p[2].row, 1);
    CHECK_EQ((int)p[3].row, 1);   // follows the pressure, not back-filled to row 0
}

static void test_pack_respects_maxout_and_empty_input() {
    KindleSlotList l;
    l.clear();
    for (int i = 0; i < 8; i++) l.add("s", "pm25", nullptr, KSLOT_SMALL);

    KdSlotPlacement p[4];
    CHECK_EQ(kdSlotsPack(l, nullptr, p, 4), 4);
    CHECK_EQ(kdSlotsPack(l, nullptr, p, 0), 0);
    CHECK_EQ(kdSlotsPack(l, nullptr, nullptr, 4), 0);

    KindleSlotList empty;
    empty.clear();
    CHECK_EQ(kdSlotsPack(empty, nullptr, p, 4), 0);
    CHECK_EQ(kdSlotsRowCount(p, 0), 0);
}

// Every size must tile a row exactly, or a combination somewhere leaves a
// sliver that the renderers would each round differently.
static void test_every_size_divides_a_row() {
    for (uint8_t s = 0; s < KSLOT_SIZE_COUNT; s++) {
        const uint8_t u = kdSlotUnits(s);
        CHECK(u > 0);
        CHECK(u <= KSLOT_ROW_UNITS);
        CHECK_EQ(KSLOT_ROW_UNITS % u, 0);
    }
}

// ---------------------------------------------------------------------------
// Clamping
// ---------------------------------------------------------------------------
static void test_clamp_drops_unusable_slots() {
    KindleSlotList l;
    l.clear();
    l.count = 5;
    strcpy(l.slot[0].sensorId, "a"); strcpy(l.slot[0].metric, "temperature");
    // 1: no metric — unusable
    strcpy(l.slot[1].sensorId, "b");
    // 2: no sensor — unusable
    strcpy(l.slot[2].metric, "humidity");
    strcpy(l.slot[3].sensorId, "c"); strcpy(l.slot[3].metric, "pm25");
    strcpy(l.slot[4].sensorId, "d"); strcpy(l.slot[4].metric, "aqi");

    kdSlotsClamp(l);

    CHECK_EQ(l.count, 3);
    CHECK_STREQ(l.slot[0].sensorId, "a");
    CHECK_STREQ(l.slot[1].sensorId, "c");   // closed up, order kept
    CHECK_STREQ(l.slot[2].sensorId, "d");
}

static void test_clamp_bounds_everything_a_renderer_reads() {
    KindleSlotList l;
    l.clear();
    l.count = 1;
    strcpy(l.slot[0].sensorId, "a");
    strcpy(l.slot[0].metric,   "temperature");
    l.slot[0].size     = 200;      // not one of the four
    l.slot[0].decimals = 200;
    l.slot[0].flags    = 0xFF;     // bits nothing defines

    kdSlotsClamp(l);

    CHECK(l.slot[0].size < KSLOT_SIZE_COUNT);
    CHECK(l.slot[0].decimals <= 3 || l.slot[0].decimals == KSLOT_DECIMALS_AUTO);
    CHECK_EQ((int)(l.slot[0].flags & ~KSLOTF_ALL), 0);

    // A count from a corrupt file must not walk the renderer off the array.
    KindleSlotList big;
    big.clear();
    big.count = 9999;
    kdSlotsClamp(big);
    CHECK(big.count >= 0 && big.count <= KindleSlotList::CAP);

    KindleSlotList neg;
    neg.clear();
    neg.count = -3;
    kdSlotsClamp(neg);
    CHECK_EQ(neg.count, 0);
}

static void test_add_refuses_past_the_cap() {
    KindleSlotList l;
    l.clear();
    for (int i = 0; i < KindleSlotList::CAP; i++)
        CHECK(l.add("s", "pm25", nullptr, KSLOT_SMALL));
    CHECK(!l.add("s", "pm25", nullptr, KSLOT_SMALL));
    CHECK_EQ(l.count, KindleSlotList::CAP);

    // And refuses the ones that would be dead on arrival.
    KindleSlotList m;
    m.clear();
    CHECK(!m.add("", "temperature", nullptr, KSLOT_HERO));
    CHECK(!m.add("s", "", nullptr, KSLOT_HERO));
    CHECK(!m.add(nullptr, "temperature", nullptr, KSLOT_HERO));
    CHECK_EQ(m.count, 0);
}

// A name longer than the field is truncated, not written past the end.
static void test_long_names_are_truncated_not_overflowed() {
    KindleSlotList l;
    l.clear();
    const char* longId = "a_very_long_sensor_identifier_indeed";
    l.add(longId, "temperature", "a label that is far too long to fit", KSLOT_SMALL);

    CHECK_EQ(l.count, 1);
    CHECK_EQ((int)strlen(l.slot[0].sensorId), (int)sizeof(l.slot[0].sensorId) - 1);
    CHECK_EQ((int)strlen(l.slot[0].label),    (int)sizeof(l.slot[0].label) - 1);
    CHECK_EQ((int)l.slot[0].sensorId[sizeof(l.slot[0].sensorId) - 1], 0);
    CHECK_EQ((int)l.slot[0].label[sizeof(l.slot[0].label) - 1], 0);
}

int main() {
    RUN(test_defaults_reproduce_the_old_dashboard);
    RUN(test_labels_come_from_the_table_or_the_slot);
    RUN(test_decimals_come_from_the_metric_unless_set);
    RUN(test_a_row_never_overflows);
    RUN(test_a_hero_owns_its_row);
    RUN(test_an_absent_reading_leaves_no_gap);
    RUN(test_packing_preserves_order);
    RUN(test_pack_respects_maxout_and_empty_input);
    RUN(test_every_size_divides_a_row);
    RUN(test_clamp_drops_unusable_slots);
    RUN(test_clamp_bounds_everything_a_renderer_reads);
    RUN(test_add_refuses_past_the_cap);
    RUN(test_long_names_are_truncated_not_overflowed);
    return SUMMARY();
}
