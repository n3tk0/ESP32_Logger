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
    // Two survivors share the row rather than leaving the last third white:
    // six twelfths each, so the pressure starts at the halfway mark.
    CHECK_EQ((int)p[1].col, 6);
    CHECK_EQ((int)p[0].units, 6);
    CHECK_EQ((int)p[1].units, 6);
    CHECK_EQ(kdSlotsRowCount(p, n), 1);
}

// A row always ends flush against the right margin. The old hardwired design
// used both columns fully all the way down; a flow that stops two twelfths
// short on every other row reads as a missing value rather than as space.
static void test_rows_end_flush() {
    struct Case { uint8_t sizes[6]; int count; uint8_t firstRow; };
    static const Case cases[] = {
        { { KSLOT_MEDIUM, KSLOT_MEDIUM, 0, 0, 0, 0 }, 2, KSLOT_ROW_UNITS },
        { { KSLOT_SMALL,  KSLOT_SMALL,  0, 0, 0, 0 }, 2, KSLOT_ROW_UNITS },
        { { KSLOT_LARGE,  KSLOT_SMALL,  0, 0, 0, 0 }, 2, KSLOT_ROW_UNITS },
        { { KSLOT_SMALL,  0, 0, 0, 0, 0 },            1, KSLOT_ROW_UNITS },
        { { KSLOT_HERO,   KSLOT_MEDIUM, KSLOT_MEDIUM, KSLOT_LARGE,
            KSLOT_MEDIUM, KSLOT_MEDIUM },             6,
          (uint8_t)(KSLOT_ROW_UNITS - KSLOT_CLOCK_UNITS) },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        KindleSlotList l;
        l.clear();
        for (int i = 0; i < cases[c].count; i++)
            l.add("s", "pm25", nullptr, cases[c].sizes[i]);

        KdSlotPlacement p[KindleSlotList::CAP];
        const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP,
                                  cases[c].firstRow);
        CHECK_EQ(n, cases[c].count);

        for (int i = 0; i < n; ) {
            int j = i, used = 0;
            while (j < n && p[j].row == p[i].row) { used += p[j].units; j++; }
            const int cap = (p[i].row == 0) ? cases[c].firstRow : KSLOT_ROW_UNITS;
            CHECK_EQ(used, cap);                 // flush, neither short nor over
            CHECK_EQ((int)p[i].col, 0);          // and still laid left to right
            for (int k = i + 1; k < j; k++)
                CHECK_EQ((int)p[k].col, (int)p[k - 1].col + (int)p[k - 1].units);
            i = j;
        }
    }
}

// Two rows holding the same number of readings put their columns in the same
// places, whatever sizes those readings are. This is the alignment down a page
// that made the old hardwired design read as a page rather than as six values
// scattered over one.
static void test_rows_of_equal_count_share_their_columns() {
    KindleSlotList l;
    l.clear();
    l.add("s", "humidity",    nullptr, KSLOT_MEDIUM);   // row 1: medium, medium
    l.add("s", "pressure",    nullptr, KSLOT_MEDIUM);
    l.add("s", "temperature", nullptr, KSLOT_LARGE);    // row 2: large, medium
    l.add("s", "humidity",    nullptr, KSLOT_MEDIUM);

    // Full-width rows on both, so the comparison is between two rows of the
    // same capacity — row 0 shares with the clock and is narrower by design.
    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP);
    CHECK_EQ(n, 4);

    // Both pairs land two across.
    CHECK_EQ((int)p[0].row, (int)p[1].row);
    CHECK_EQ((int)p[2].row, (int)p[3].row);
    CHECK((int)p[2].row > (int)p[0].row);

    // The second column starts at the same twelfth on both, though one row
    // holds a large and the other does not.
    CHECK_EQ((int)p[1].col, (int)p[3].col);
    CHECK_EQ((int)p[1].col, 6);
}

// A row's slots are never zero twelfths wide, however many of them there are.
// A zero-width column is an invisible reading that still cost a list entry.
static void test_no_column_is_zero_wide() {
    for (uint8_t first = 1; first <= KSLOT_ROW_UNITS; first++) {
        KindleSlotList l;
        l.clear();
        for (int i = 0; i < KindleSlotList::CAP; i++)
            l.add("s", "pm25", nullptr, KSLOT_SMALL);

        KdSlotPlacement p[KindleSlotList::CAP];
        const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP, first);
        for (int i = 0; i < n; i++) {
            CHECK(p[i].units > 0);
            CHECK((int)p[i].col + (int)p[i].units <=
                  ((p[i].row == 0) ? (int)first : (int)KSLOT_ROW_UNITS));
        }
    }
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
    // And on the narrowed first row, where the clock has taken half.
    const uint8_t half = KSLOT_ROW_UNITS - KSLOT_CLOCK_UNITS;
    for (uint8_t s = 0; s < KSLOT_SIZE_COUNT; s++) {
        const uint8_t u = kdSlotUnits(s, half);
        CHECK(u > 0);
        CHECK(u <= half);
    }
    // A width is never zero however narrow the row gets — a zero-width slot
    // would be an invisible reading that still consumed a list entry.
    for (uint8_t r = 1; r <= KSLOT_ROW_UNITS; r++)
        for (uint8_t s = 0; s < KSLOT_SIZE_COUNT; s++)
            CHECK(kdSlotUnits(s, r) > 0);
}

// ---------------------------------------------------------------------------
// Sharing row 0 with the clock
// ---------------------------------------------------------------------------
// The clock is not a reading, so it is not a slot — but both renderers draw it
// over the top-right of the first row, and the packer has to know. Before this,
// a hero claimed all twelve twelfths of row 0 and the outdoor temperature ran
// underneath the clock as soon as the value got wide.
static void test_the_clock_narrows_the_first_row() {
    const uint8_t half = KSLOT_ROW_UNITS - KSLOT_CLOCK_UNITS;

    KindleSlotList l;
    l.clear();
    l.add("s", "temperature", nullptr, KSLOT_HERO);     // row 0, beside the clock
    l.add("s", "pressure",    nullptr, KSLOT_MEDIUM);   // row 1, full width
    l.add("s", "humidity",    nullptr, KSLOT_MEDIUM);
    l.add("s", "pm25",        nullptr, KSLOT_MEDIUM);

    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP, half);
    CHECK_EQ(n, 4);

    // The hero takes the half it has, not the whole width.
    CHECK_EQ((int)p[0].row, 0);
    CHECK_EQ((int)p[0].col, 0);
    CHECK_EQ((int)p[0].units, (int)half);

    // Everything after it is on a full-width row and packs three across.
    CHECK_EQ((int)p[1].row, 1);
    CHECK_EQ((int)p[1].units, 4);
    CHECK_EQ((int)p[2].row, 1);
    CHECK_EQ((int)p[3].row, 1);
    CHECK_EQ((int)p[3].col, 8);
    CHECK_EQ(kdSlotsRowCount(p, n), 2);
}

// Row 0 holds only what fits beside the clock; the rest moves down.
static void test_small_slots_fill_only_half_of_row_zero() {
    const uint8_t half = KSLOT_ROW_UNITS - KSLOT_CLOCK_UNITS;   // 6

    KindleSlotList l;
    l.clear();
    for (int i = 0; i < 5; i++) l.add("s", "pm25", nullptr, KSLOT_SMALL);

    KdSlotPlacement p[KindleSlotList::CAP];
    const int n = kdSlotsPack(l, nullptr, p, KindleSlotList::CAP, half);
    CHECK_EQ(n, 5);

    // A small is a quarter of the row it is on: 1 unit of six on row 0, 3 of
    // twelve after that. Six/one = six would fit, but the first row's capacity
    // is what stops it running under the clock.
    int onRow0 = 0, used0 = 0;
    for (int i = 0; i < n; i++)
        if (p[i].row == 0) { onRow0++; used0 += p[i].units; }
    CHECK(used0 <= (int)half);
    CHECK(onRow0 >= 1);

    // Nothing on any row exceeds that row's capacity.
    int used[8] = {0};
    for (int i = 0; i < n; i++) used[p[i].row] += p[i].units;
    CHECK(used[0] <= (int)half);
    for (int r = 1; r < 8; r++) CHECK(used[r] <= KSLOT_ROW_UNITS);
}

// The default is unchanged, so every existing caller and test still describes
// a full-width first row.
static void test_packing_defaults_to_a_full_first_row() {
    KindleSlotList l;
    l.clear();
    l.add("s", "temperature", nullptr, KSLOT_HERO);

    KdSlotPlacement a[4], b[4];
    const int na = kdSlotsPack(l, nullptr, a, 4);
    const int nb = kdSlotsPack(l, nullptr, b, 4, KSLOT_ROW_UNITS);
    CHECK_EQ(na, nb);
    CHECK_EQ((int)a[0].units, (int)b[0].units);
    CHECK_EQ((int)a[0].units, (int)KSLOT_ROW_UNITS);

    // An out-of-range capacity falls back to the full row rather than
    // producing a layout nobody asked for.
    kdSlotsPack(l, nullptr, b, 4, 0);
    CHECK_EQ((int)b[0].units, (int)KSLOT_ROW_UNITS);
    kdSlotsPack(l, nullptr, b, 4, 99);
    CHECK_EQ((int)b[0].units, (int)KSLOT_ROW_UNITS);
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
    RUN(test_rows_end_flush);
    RUN(test_rows_of_equal_count_share_their_columns);
    RUN(test_no_column_is_zero_wide);
    RUN(test_packing_preserves_order);
    RUN(test_pack_respects_maxout_and_empty_input);
    RUN(test_every_size_divides_a_row);
    RUN(test_the_clock_narrows_the_first_row);
    RUN(test_small_slots_fill_only_half_of_row_zero);
    RUN(test_packing_defaults_to_a_full_first_row);
    RUN(test_clamp_drops_unusable_slots);
    RUN(test_clamp_bounds_everything_a_renderer_reads);
    RUN(test_add_refuses_past_the_cap);
    RUN(test_long_names_are_truncated_not_overflowed);
    return SUMMARY();
}
