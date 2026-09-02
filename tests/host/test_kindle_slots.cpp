// Host unit tests for src/web/KindleSlots.h
//
// The nine places are what the dashboard draws, so the things worth checking
// are the ones that decide whether a reader sees a sensible page or a broken
// one:
//
//   • the defaults reproduce the dashboard as it was before any of this was
//     configurable — an upgrade must not rearrange a page somebody has been
//     reading for months;
//   • a place with no reading leaves NO gap, because that is the whole point of
//     the BMP280 case: no humidity measured, no humidity in the grid;
//   • the indoor row comes back as two when only two are configured, which is
//     how "three fields, or two" is a setting rather than a mode;
//   • the zone keys round-trip, since they are what the file and the API are
//     addressed by;
//   • anything arriving from a file is clamped before a renderer sees it.
#include <stdint.h>
#include <string.h>

#include "src/web/KindleSlots.h"
#include "check.h"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
static void test_defaults_reproduce_the_old_dashboard() {
    KindleZones k;
    kdZonesDefault(k, "balcony", "livingroom");

    // The headline: outdoor temperature, with the humidity beside it.
    CHECK_STREQ(k.z[KZ_HERO].sensorId, "balcony");
    CHECK_STREQ(k.z[KZ_HERO].metric,   "temperature");
    CHECK((k.z[KZ_HERO].flags & KSLOTF_BOLD) != 0);
    CHECK((k.z[KZ_HERO].flags & KSLOTF_AGE)  != 0);
    CHECK_STREQ(k.z[KZ_BIG].sensorId, "balcony");
    CHECK_STREQ(k.z[KZ_BIG].metric,   "humidity");

    // The grid: pressure with its tendency, then the dew point.
    CHECK_STREQ(k.z[KZ_G1].metric, "pressure");
    CHECK((k.z[KZ_G1].flags & KSLOTF_TREND) != 0);
    CHECK_STREQ(k.z[KZ_G2].metric, "dew_point");

    // THE LAST TWO GRID PLACES ARRIVE EMPTY, deliberately. A dashboard that
    // shipped showing a metric the hardware does not have would be showing a
    // dash, which is the fault this design exists to remove.
    CHECK(!k.z[KZ_G3].used());
    CHECK(!k.z[KZ_G4].used());

    // The indoor row.
    CHECK_STREQ(k.z[KZ_IN1].sensorId, "livingroom");
    CHECK_STREQ(k.z[KZ_IN1].metric,   "temperature");
    CHECK_STREQ(k.z[KZ_IN2].metric,   "humidity");
    CHECK_STREQ(k.z[KZ_IN3].metric,   "aqi");

    CHECK_EQ(k.configured(), 7);

    // An empty sensor id must not produce a place keyed on "" — every reading
    // would miss and the page would be blank with no clue why.
    KindleZones d;
    kdZonesDefault(d, "", nullptr);
    CHECK(d.z[KZ_HERO].sensorId[0] != '\0');
    CHECK(d.z[KZ_IN1].sensorId[0]  != '\0');

    // And the headings fall back rather than rendering as nothing.
    CHECK(kdGroupOutLabel(d)[0] != '\0');
    CHECK(kdGroupInLabel(d)[0]  != '\0');
}

static void test_group_headings_are_overridable() {
    KindleZones k;
    kdZonesDefault(k, "a", "b");
    const char* builtinOut = kdGroupOutLabel(k);

    strcpy(k.groupOut, "Балкон");
    CHECK_STREQ(kdGroupOutLabel(k), "Балкон");
    CHECK_STREQ(kdGroupInLabel(k), builtinOut[0] ? kdGroupInLabel(k) : "");

    // Cleared goes back to the built-in rather than to an empty heading.
    k.groupOut[0] = '\0';
    CHECK_STREQ(kdGroupOutLabel(k), builtinOut);
}

// ---------------------------------------------------------------------------
// The keys the file and the API are addressed by
// ---------------------------------------------------------------------------
static void test_zone_keys_round_trip() {
    for (uint8_t z = 0; z < KZ_COUNT; z++) {
        const char* k = kdZoneKey(z);
        CHECK(k[0] != '\0');
        CHECK_EQ((int)kdZoneFromKey(k), (int)z);
    }
    // Every key distinct — two places sharing one would silently overwrite
    // each other on load.
    for (uint8_t a = 0; a < KZ_COUNT; a++)
        for (uint8_t b = (uint8_t)(a + 1); b < KZ_COUNT; b++)
            CHECK(strcmp(kdZoneKey(a), kdZoneKey(b)) != 0);

    // Anything else is refused rather than guessed at. A key from a later
    // build must not land in whichever place happens to be first.
    CHECK_EQ((int)kdZoneFromKey("g5"), (int)KZ_COUNT);
    CHECK_EQ((int)kdZoneFromKey(""),   (int)KZ_COUNT);
    CHECK_EQ((int)kdZoneFromKey(nullptr), (int)KZ_COUNT);
    CHECK_STREQ(kdZoneKey(KZ_COUNT), "");
    CHECK_STREQ(kdZoneKey(200), "");
}

static void test_groups_are_named_correctly() {
    CHECK(kdZoneIsIndoor(KZ_IN1));
    CHECK(kdZoneIsIndoor(KZ_IN3));
    CHECK(!kdZoneIsIndoor(KZ_HERO));
    CHECK(!kdZoneIsIndoor(KZ_G4));

    CHECK(kdZoneIsGrid(KZ_G1));
    CHECK(kdZoneIsGrid(KZ_G4));
    CHECK(!kdZoneIsGrid(KZ_BIG));
    CHECK(!kdZoneIsGrid(KZ_IN1));
}

// ---------------------------------------------------------------------------
// Labels and decimals
// ---------------------------------------------------------------------------
static void test_labels_come_from_the_table_or_the_place() {
    KindleZones k;
    k.clear();
    k.set(KZ_G1, "s", "pm25");
    k.set(KZ_G2, "s", "temperature", "Спалня");
    k.set(KZ_G3, "s", "unlisted_metric");

    // The table's name, not the metric id.
    CHECK_STREQ(kdSlotLabel(k.z[KZ_G1]), "PM2.5");
    // The reader's own name beats the table.
    CHECK_STREQ(kdSlotLabel(k.z[KZ_G2]), "Спалня");
    // Unlisted falls back to the metric — not pretty, but honest, where a
    // table that quietly rendered nothing would be worse.
    CHECK_STREQ(kdSlotLabel(k.z[KZ_G3]), "unlisted_metric");
}

static void test_units_come_from_the_table_then_the_reading() {
    KindleZones k;
    k.clear();
    k.set(KZ_G1, "s", "temperature");
    k.set(KZ_G2, "s", "uva");

    // The table overrides the pipeline's machine-readable unit.
    CHECK_STREQ(kdSlotUnit(k.z[KZ_G1], "C"), "°");
    // With no table entry for the unit, the sensor's own is used.
    CHECK_STREQ(kdSlotUnit(k.z[KZ_G2], "index"), "index");
    // And a reading with no unit prints none rather than "(null)".
    CHECK_STREQ(kdSlotUnit(k.z[KZ_G2], nullptr), "");
}

static void test_decimals_come_from_the_metric_unless_set() {
    KindleZones k;
    k.clear();
    k.set(KZ_G1, "s", "temperature");
    k.set(KZ_G2, "s", "humidity");
    k.set(KZ_G3, "s", "battery_voltage");
    k.set(KZ_G4, "s", "temperature", nullptr, 0);

    CHECK_EQ((int)kdSlotDecimals(k.z[KZ_G1]), 1);
    CHECK_EQ((int)kdSlotDecimals(k.z[KZ_G2]), 0);
    CHECK_EQ((int)kdSlotDecimals(k.z[KZ_G3]), 2);
    CHECK_EQ((int)kdSlotDecimals(k.z[KZ_G4]), 0);   // the override wins

    // A nonsense override is clamped rather than passed to a printf as %.*f
    // with a width nobody budgeted a buffer for.
    KindleSlot wild{};
    strcpy(wild.metric, "temperature");
    wild.decimals = 99;
    CHECK_EQ((int)kdSlotDecimals(wild), 3);
}

// ---------------------------------------------------------------------------
// Closing up behind an empty place
// ---------------------------------------------------------------------------
// The case the whole feature exists for.
static void test_an_absent_reading_leaves_no_gap() {
    KindleZones k;
    k.clear();
    k.set(KZ_G1, "bmp280", "pressure");
    k.set(KZ_G2, "bmp280", "humidity");    // a BMP280 has none
    k.set(KZ_G3, "bmp280", "dew_point");

    bool visible[KZ_COUNT] = {false};
    visible[KZ_G1] = true;
    visible[KZ_G2] = false;
    visible[KZ_G3] = true;

    uint8_t used[KZ_GRID_COUNT];
    const int n = kdGridUsed(k, visible, used);

    // Two cells, and the dew point has moved UP into the place the humidity
    // would have had — it does not sit third with a hole in front of it.
    CHECK_EQ(n, 2);
    CHECK_EQ((int)used[0], (int)KZ_G1);
    CHECK_EQ((int)used[1], (int)KZ_G3);
}

static void test_an_unconfigured_place_is_skipped_too() {
    KindleZones k;
    k.clear();
    k.set(KZ_G2, "s", "pm25");     // G1 left empty on purpose
    k.set(KZ_G4, "s", "co2");

    uint8_t used[KZ_GRID_COUNT];
    const int n = kdGridUsed(k, nullptr, used);   // nullptr = everything reports
    CHECK_EQ(n, 2);
    CHECK_EQ((int)used[0], (int)KZ_G2);
    CHECK_EQ((int)used[1], (int)KZ_G4);
}

// "Three fields, or two" is a setting rather than a mode: leave the third
// empty and two come back.
static void test_the_indoor_row_can_be_two() {
    KindleZones k;
    k.clear();
    k.set(KZ_IN1, "in", "temperature");
    k.set(KZ_IN2, "in", "humidity");

    uint8_t used[KZ_INDOOR_COUNT];
    CHECK_EQ(kdIndoorUsed(k, nullptr, used), 2);
    CHECK_EQ((int)used[0], (int)KZ_IN1);
    CHECK_EQ((int)used[1], (int)KZ_IN2);

    k.set(KZ_IN3, "in", "aqi");
    CHECK_EQ(kdIndoorUsed(k, nullptr, used), 3);

    // And one quiet sensor takes it back down without disturbing the order.
    bool visible[KZ_COUNT];
    for (int i = 0; i < KZ_COUNT; i++) visible[i] = true;
    visible[KZ_IN2] = false;
    CHECK_EQ(kdIndoorUsed(k, visible, used), 2);
    CHECK_EQ((int)used[0], (int)KZ_IN1);
    CHECK_EQ((int)used[1], (int)KZ_IN3);
}

// The two groups never bleed into one another. An off-by-one in the walk would
// put an indoor reading in the outdoor grid, which reads as a wrong number
// rather than as a layout bug.
static void test_the_groups_do_not_bleed() {
    KindleZones k;
    k.clear();
    for (int i = 0; i < KZ_COUNT; i++) k.set((uint8_t)i, "s", "pm25");

    uint8_t grid[KZ_GRID_COUNT];
    CHECK_EQ(kdGridUsed(k, nullptr, grid), KZ_GRID_COUNT);
    for (int i = 0; i < KZ_GRID_COUNT; i++) CHECK(kdZoneIsGrid(grid[i]));

    uint8_t in[KZ_INDOOR_COUNT];
    CHECK_EQ(kdIndoorUsed(k, nullptr, in), KZ_INDOOR_COUNT);
    for (int i = 0; i < KZ_INDOOR_COUNT; i++) CHECK(kdZoneIsIndoor(in[i]));

    // Nothing configured at all is zero of each, not a walk off the end.
    KindleZones empty;
    empty.clear();
    CHECK_EQ(kdGridUsed(empty, nullptr, grid), 0);
    CHECK_EQ(kdIndoorUsed(empty, nullptr, in), 0);
    CHECK_EQ(empty.configured(), 0);
}

static void test_used_refuses_a_bad_call() {
    KindleZones k;
    kdZonesDefault(k, "a", "b");
    uint8_t out[KZ_COUNT];

    CHECK_EQ(kdZonesUsed(k, nullptr, KZ_G1, 0, out), 0);
    CHECK_EQ(kdZonesUsed(k, nullptr, KZ_G1, KZ_GRID_COUNT, nullptr), 0);
    // A walk that would run past the end stops at it rather than reading
    // whatever follows the array.
    CHECK(kdZonesUsed(k, nullptr, KZ_IN3, 8, out) <= 1);
}

// ---------------------------------------------------------------------------
// Clamping
// ---------------------------------------------------------------------------
static void test_clamp_empties_a_half_filled_place() {
    KindleZones k;
    k.clear();
    // A sensor with no metric, and a metric with no sensor. Both arrive from a
    // form where somebody filled one box; neither can produce a reading.
    strcpy(k.z[KZ_G1].sensorId, "s");
    strcpy(k.z[KZ_G2].metric,   "pm25");
    k.set(KZ_G3, "s", "co2");

    kdZonesClamp(k);

    CHECK(!k.z[KZ_G1].used());
    CHECK(!k.z[KZ_G2].used());
    CHECK(k.z[KZ_G3].used());
    // And emptied means EMPTIED — a leftover sensor id in a place the reader
    // cleared would come back the next time they set a metric on it.
    CHECK_STREQ(k.z[KZ_G1].sensorId, "");
    CHECK_STREQ(k.z[KZ_G2].metric,   "");
    CHECK_EQ(k.configured(), 1);
}

static void test_clamp_bounds_everything_a_renderer_reads() {
    KindleZones k;
    k.clear();
    k.set(KZ_HERO, "s", "temperature");
    k.z[KZ_HERO].decimals = 9;
    k.z[KZ_HERO].flags    = 0xFF;

    kdZonesClamp(k);

    CHECK_EQ((int)k.z[KZ_HERO].decimals, 3);
    CHECK_EQ((int)k.z[KZ_HERO].flags, (int)KSLOTF_ALL);

    // AUTO is not a number and must survive the clamp untouched — turning it
    // into 3 would silently give every place three decimals.
    KindleZones a;
    a.clear();
    a.set(KZ_HERO, "s", "temperature");   // defaults to AUTO
    kdZonesClamp(a);
    CHECK_EQ((int)a.z[KZ_HERO].decimals, (int)KSLOT_DECIMALS_AUTO);
}

static void test_set_refuses_what_it_cannot_store() {
    KindleZones k;
    k.clear();

    CHECK(!k.set(KZ_COUNT, "s", "pm25"));      // no such place
    CHECK(!k.set(200,      "s", "pm25"));
    CHECK(!k.set(KZ_G1, nullptr, "pm25"));
    CHECK(!k.set(KZ_G1, "s", nullptr));
    CHECK(!k.set(KZ_G1, "", "pm25"));          // empty is not a sensor id
    CHECK(!k.z[KZ_G1].used());
    CHECK_EQ(k.configured(), 0);

    CHECK(k.set(KZ_G1, "s", "pm25"));
    CHECK_EQ(k.configured(), 1);
}

static void test_long_names_are_truncated_not_overflowed() {
    // Longer than every buffer in KindleSlot, and longer than a JSON parser
    // would have bothered to check.
    const char* huge = "0123456789012345678901234567890123456789";

    KindleZones k;
    k.clear();
    CHECK(k.set(KZ_HERO, huge, huge, huge));

    CHECK_EQ(strlen(k.z[KZ_HERO].sensorId), sizeof(k.z[KZ_HERO].sensorId) - 1);
    CHECK_EQ(strlen(k.z[KZ_HERO].metric),   sizeof(k.z[KZ_HERO].metric)   - 1);
    CHECK_EQ(strlen(k.z[KZ_HERO].label),    sizeof(k.z[KZ_HERO].label)    - 1);

    // And a clamp over a buffer somebody filled to the brim without a
    // terminator leaves it terminated.
    memset(k.z[KZ_G1].sensorId, 'x', sizeof(k.z[KZ_G1].sensorId));
    memset(k.z[KZ_G1].metric,   'y', sizeof(k.z[KZ_G1].metric));
    memset(k.z[KZ_G1].label,    'z', sizeof(k.z[KZ_G1].label));
    kdZonesClamp(k);
    CHECK_EQ(strlen(k.z[KZ_G1].sensorId), sizeof(k.z[KZ_G1].sensorId) - 1);
    CHECK_EQ(strlen(k.z[KZ_G1].metric),   sizeof(k.z[KZ_G1].metric)   - 1);
    CHECK_EQ(strlen(k.z[KZ_G1].label),    sizeof(k.z[KZ_G1].label)    - 1);
}

// Every metric in the style table has to be usable: a label that is empty
// renders as nothing, and a decimal count above three overruns the buffers
// kdSlotDecimals() is trusted to have bounded.
static void test_the_metric_table_is_sane() {
    for (int i = 0; i < KD_METRIC_STYLE_COUNT; i++) {
        const KdMetricStyle& m = KD_METRIC_STYLE[i];
        CHECK(m.metric && m.metric[0]);
        CHECK(m.label  && m.label[0]);
        CHECK(m.decimals <= 3);
        // Every metric name must fit KindleSlot::metric, or a place configured
        // from the table's own list would be stored truncated and never match.
        CHECK(strlen(m.metric) < sizeof(((KindleSlot*)nullptr)->metric));
    }
    // No duplicates: the lookup returns the first, so a second entry for one
    // metric is a rule nobody can see not being applied.
    for (int a = 0; a < KD_METRIC_STYLE_COUNT; a++)
        for (int b = a + 1; b < KD_METRIC_STYLE_COUNT; b++)
            CHECK(strcmp(KD_METRIC_STYLE[a].metric, KD_METRIC_STYLE[b].metric) != 0);

    CHECK(kdMetricStyle(nullptr) == nullptr);
    CHECK(kdMetricStyle("")      == nullptr);
    CHECK(kdMetricStyle("nope")  == nullptr);
}

int main() {
    RUN(test_defaults_reproduce_the_old_dashboard);
    RUN(test_group_headings_are_overridable);
    RUN(test_zone_keys_round_trip);
    RUN(test_groups_are_named_correctly);
    RUN(test_labels_come_from_the_table_or_the_place);
    RUN(test_units_come_from_the_table_then_the_reading);
    RUN(test_decimals_come_from_the_metric_unless_set);
    RUN(test_an_absent_reading_leaves_no_gap);
    RUN(test_an_unconfigured_place_is_skipped_too);
    RUN(test_the_indoor_row_can_be_two);
    RUN(test_the_groups_do_not_bleed);
    RUN(test_used_refuses_a_bad_call);
    RUN(test_clamp_empties_a_half_filled_place);
    RUN(test_clamp_bounds_everything_a_renderer_reads);
    RUN(test_set_refuses_what_it_cannot_store);
    RUN(test_long_names_are_truncated_not_overflowed);
    RUN(test_the_metric_table_is_sane);
    return SUMMARY();
}
