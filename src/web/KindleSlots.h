// ============================================================================
// src/web/KindleSlots.h — what the e-ink dashboard shows, and where
//
// The page used to be six hardwired readings: the outdoor sensor's
// temperature, humidity and pressure, and the indoor sensor's temperature,
// humidity and AQI. That is a fine dashboard for exactly one hardware
// configuration. This firmware has twenty sensor plugins between them
// producing twenty-nine distinct metrics, and the page could reach six.
//
// The cost was not only the metrics it could not show. It was the ones it
// insisted on: a BMP280 outdoors measures no humidity, so the humidity row
// rendered a dash forever in a space nothing could use, and an SDS011 on the
// balcony had nowhere at all to put PM2.5.
//
// So the layout is a LIST rather than a shape. Each slot names a sensor, a
// metric and a size; the renderers walk the list in order and pack it into
// rows. Adding a reading is adding a slot, and a slot whose sensor is not
// reporting simply is not drawn — the row closes up behind it.
//
// SIZE AND ORDER, NOT X AND Y
// ---------------------------
// A slot does not carry coordinates, and that is deliberate rather than a
// simplification. The dashboard is rendered at 600x800 on a Kindle 7 and at
// 1072x1448 on a Paperwhite, by two entirely different renderers — a CSS page
// and an FBInk shell script. Pixel positions chosen for one are wrong for the
// other, and asking the reader to place every value twice, by hand, in a
// coordinate system they cannot see, is not flexibility. Order plus size is
// the same freedom expressed in a form that survives both screens: the sizes
// are the ones the layout was tuned at, and the packing keeps the margins and
// rhythm that make an e-ink page legible at a glance.
//
// NO ARDUINO IN HERE, for the reason NodeTable.h has none: the packing, the
// defaults and the label rules are the parts most likely to be wrong, and
// tests/host/test_kindle_slots.cpp can exercise all of them on the build host.
// Loading and saving the list needs ArduinoJson and a filesystem, so that
// lives in KindleSlots.cpp.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "DashboardStrings.h"   // KD_T — a plain macro, no Arduino behind it

// DashboardStrings.h defines KD_T only inside FEATURE_KINDLE_DASHBOARD, and it
// reaches that verdict through setup.h. The fallback here is the same one
// RefreshCadence.h carries and for the same reason: a host test includes this
// header on its own, and a label table is not a thing that should need the
// whole build configuration to compile.
#ifndef KD_T
#  define KD_T(en, bg) en
#endif

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------
// Four, and the widths are twelfths so every combination tiles a row exactly.
// A HERO takes the full width and always starts one; the others pack.
enum KindleSlotSize : uint8_t {
    KSLOT_HERO   = 0,   ///< the glance value — full width, largest type
    KSLOT_LARGE  = 1,   ///< half a row
    KSLOT_MEDIUM = 2,   ///< a third
    KSLOT_SMALL  = 3,   ///< a quarter
    KSLOT_SIZE_COUNT
};

/// Row width in twelfths, so LARGE+MEDIUM+SMALL cannot silently overflow.
static const uint8_t KSLOT_ROW_UNITS = 12;

static inline uint8_t kdSlotUnits(uint8_t size) {
    switch (size) {
        case KSLOT_HERO:   return 12;
        case KSLOT_LARGE:  return 6;
        case KSLOT_MEDIUM: return 4;
        case KSLOT_SMALL:  return 3;
        default:           return 4;
    }
}

// ---------------------------------------------------------------------------
// Per-slot options
// ---------------------------------------------------------------------------
constexpr uint8_t KSLOTF_BOLD     = 0x01;  ///< draw the value bold
constexpr uint8_t KSLOTF_UNIT     = 0x02;  ///< append the reading's own unit
constexpr uint8_t KSLOTF_AGE      = 0x04;  ///< append "· 4m" when it is stale
constexpr uint8_t KSLOTF_TREND    = 0x08;  ///< append the 3 h tendency arrow
constexpr uint8_t KSLOTF_ALL      = 0x0F;

/// 0xFF in `decimals` means "use the metric's own convention".
static const uint8_t KSLOT_DECIMALS_AUTO = 0xFF;

// ---------------------------------------------------------------------------
// One slot
// ---------------------------------------------------------------------------
// The id and metric buffers match SensorReading's, so a name that fits the
// pipeline fits here — and one that does not was already being truncated
// upstream rather than by this.
struct KindleSlot {
    char    sensorId[17];   ///< "" = the slot is unused
    char    metric[17];
    char    label[17];      ///< "" = derive from the metric
    uint8_t size;           ///< KindleSlotSize
    uint8_t decimals;       ///< KSLOT_DECIMALS_AUTO or 0..3
    uint8_t flags;          ///< KSLOTF_*

    bool used() const { return sensorId[0] != '\0' && metric[0] != '\0'; }
};

// ---------------------------------------------------------------------------
// What a metric is called, and how precisely it is worth showing
// ---------------------------------------------------------------------------
// A table rather than per-slot configuration, because "pm25" should read as
// "PM2.5" on every dashboard ever configured and nobody should have to type
// that. The label is still overridable — a slot named "Спалня" beats any
// table — but the default is right often enough that most slots need nothing.
struct KdMetricStyle {
    const char* metric;
    const char* label;
    uint8_t     decimals;
};

static const KdMetricStyle KD_METRIC_STYLE[] = {
    { "temperature",   KD_T("TEMP",  "ТЕМП"),    1 },
    { "humidity",      KD_T("HUM",   "ВЛАГА"),   0 },
    { "humidity_amb",  KD_T("HUM",   "ВЛАГА"),   0 },
    { "dew_point",     KD_T("DEW",   "РОСА"),    1 },
    { "pressure",      KD_T("PRESS", "НАЛЯГ"),   0 },
    { "aqi",           "AQI",                    0 },
    { "co2",           "CO₂",                    0 },
    { "eco2",          "eCO₂",                   0 },
    { "tvoc",          "TVOC",                   0 },
    { "pm1",           "PM1",                    0 },
    { "pm25",          "PM2.5",                  0 },
    { "pm4",           "PM4",                    0 },
    { "pm10",          "PM10",                   0 },
    { "lux",           KD_T("LIGHT", "СВЕТЛ"),   0 },
    { "uva",           "UVA",                    1 },
    { "uvb",           "UVB",                    1 },
    { "rain",          KD_T("RAIN",  "ДЪЖД"),    1 },
    { "rain_rate",     KD_T("RAIN/h","ДЪЖД/ч"),  1 },
    { "rain_total",    KD_T("RAIN Σ","ДЪЖД Σ"),  1 },
    { "wind",          KD_T("WIND",  "ВЯТЪР"),   1 },
    { "wind_speed",    KD_T("WIND",  "ВЯТЪР"),   1 },
    { "wind_direction",KD_T("DIR",   "ПОСОКА"),  0 },
    { "soil_moisture", KD_T("SOIL",  "ПОЧВА"),   0 },
    { "flow_rate",     KD_T("FLOW",  "ДЕБИТ"),   1 },
    { "battery_voltage", KD_T("BATT","БАТЕРИЯ"), 2 },
    { "battery_percent", KD_T("BATT","БАТЕРИЯ"), 0 },
    { "battery_days",  KD_T("DAYS",  "ДНИ"),     0 },
};
static const int KD_METRIC_STYLE_COUNT =
    (int)(sizeof(KD_METRIC_STYLE) / sizeof(KD_METRIC_STYLE[0]));

static inline const KdMetricStyle* kdMetricStyle(const char* metric) {
    if (!metric || !*metric) return nullptr;
    for (int i = 0; i < KD_METRIC_STYLE_COUNT; i++)
        if (strcmp(KD_METRIC_STYLE[i].metric, metric) == 0) return &KD_METRIC_STYLE[i];
    return nullptr;
}

/// The label to draw for a slot: its own if set, else the metric's, else the
/// metric name itself — which is not pretty for something unlisted, but it is
/// honest, and a table that quietly rendered nothing would be worse.
static inline const char* kdSlotLabel(const KindleSlot& s) {
    if (s.label[0]) return s.label;
    const KdMetricStyle* st = kdMetricStyle(s.metric);
    return st ? st->label : s.metric;
}

static inline uint8_t kdSlotDecimals(const KindleSlot& s) {
    if (s.decimals != KSLOT_DECIMALS_AUTO) return s.decimals > 3 ? 3 : s.decimals;
    const KdMetricStyle* st = kdMetricStyle(s.metric);
    return st ? st->decimals : 1;
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------
/// Twelve. A 600x800 panel read from across a room holds a hero, a couple of
/// mid-sized values and two rows of small ones before it stops being glanceable
/// — and the cap exists so a malformed file cannot make the renderers walk off
/// the end, not to ration a scarce resource.
struct KindleSlotList {
    static constexpr int CAP = 12;

    KindleSlot slot[CAP];
    int        count = 0;

    void clear() {
        count = 0;
        for (int i = 0; i < CAP; i++) slot[i] = KindleSlot{};
    }

    bool add(const char* sensorId, const char* metric, const char* label,
             uint8_t size, uint8_t decimals = KSLOT_DECIMALS_AUTO,
             uint8_t flags = KSLOTF_UNIT) {
        if (count >= CAP || !sensorId || !metric) return false;
        KindleSlot& s = slot[count];
        s = KindleSlot{};
        strncpy(s.sensorId, sensorId, sizeof(s.sensorId) - 1);
        strncpy(s.metric,   metric,   sizeof(s.metric)   - 1);
        if (label) strncpy(s.label, label, sizeof(s.label) - 1);
        s.size     = size;
        s.decimals = decimals;
        s.flags    = flags;
        if (!s.used()) { s = KindleSlot{}; return false; }
        count++;
        return true;
    }
};

/// Bring anything that arrived from a file or a form into range.
///
/// Applied on the way IN, not at the point of use, for the reason kdSkinClamp()
/// exists: a renderer should never have to wonder whether the size byte it was
/// handed is one of the four it knows about.
static inline void kdSlotsClamp(KindleSlotList& list) {
    if (list.count < 0)             list.count = 0;
    if (list.count > KindleSlotList::CAP) list.count = KindleSlotList::CAP;

    int keep = 0;
    for (int i = 0; i < list.count; i++) {
        KindleSlot s = list.slot[i];
        s.sensorId[sizeof(s.sensorId) - 1] = '\0';
        s.metric[sizeof(s.metric) - 1]     = '\0';
        s.label[sizeof(s.label) - 1]       = '\0';
        if (!s.used()) continue;                       // drop the empty ones
        if (s.size >= KSLOT_SIZE_COUNT) s.size = KSLOT_MEDIUM;
        if (s.decimals != KSLOT_DECIMALS_AUTO && s.decimals > 3) s.decimals = 3;
        s.flags &= KSLOTF_ALL;
        list.slot[keep++] = s;
    }
    list.count = keep;
    for (int i = keep; i < KindleSlotList::CAP; i++) list.slot[i] = KindleSlot{};
}

// ---------------------------------------------------------------------------
// Packing the list into rows
// ---------------------------------------------------------------------------
/// Where one slot ends up: which row, and how many twelfths across it starts.
struct KdSlotPlacement {
    int     index;      ///< into KindleSlotList::slot
    uint8_t row;
    uint8_t col;        ///< 0..11, in twelfths from the left
    uint8_t units;      ///< width in twelfths
};

/// Greedy left-to-right packing, one pass, in list order.
///
/// ORDER IS PRESERVED EXACTLY — a slot never jumps ahead of another to fill a
/// gap. A best-fit packer would use the space better and would also mean the
/// reader's carefully chosen order rearranging itself whenever a sensor went
/// quiet, which on a glanceable display is worse than an inch of white space.
///
/// `visible` says which slots have a reading to show; a slot that has none is
/// skipped entirely and the row closes up behind it. That is the BMP280 case:
/// no humidity reading, no humidity slot, no gap where one used to be.
///
/// Returns how many placements were written, at most `maxOut`.
static inline int kdSlotsPack(const KindleSlotList& list, const bool* visible,
                              KdSlotPlacement* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;

    int     n   = 0;
    uint8_t row = 0;
    uint8_t col = 0;

    for (int i = 0; i < list.count && n < maxOut; i++) {
        if (!list.slot[i].used()) continue;
        if (visible && !visible[i]) continue;

        const uint8_t units = kdSlotUnits(list.slot[i].size);

        // A hero owns its row. Starting one mid-row would put the glance value
        // beside something small and destroy the size hierarchy that makes it
        // the glance value.
        const bool needsOwnRow = (list.slot[i].size == KSLOT_HERO);
        if ((needsOwnRow && col != 0) || (col + units > KSLOT_ROW_UNITS)) {
            row++;
            col = 0;
        }

        out[n].index = i;
        out[n].row   = row;
        out[n].col   = col;
        out[n].units = units;
        n++;

        col += units;
        if (col >= KSLOT_ROW_UNITS || needsOwnRow) { row++; col = 0; }
    }
    return n;
}

/// How many rows a packing occupies — the renderers need it to know how much
/// vertical space to reserve before the chart.
static inline int kdSlotsRowCount(const KdSlotPlacement* p, int n) {
    if (!p || n <= 0) return 0;
    return (int)p[n - 1].row + 1;
}

// ---------------------------------------------------------------------------
// The default list
// ---------------------------------------------------------------------------
/// EXACTLY WHAT THE DASHBOARD DREW BEFORE ANY OF THIS EXISTED.
///
/// A device upgrading into slots has been looking at the same page for months,
/// and it must not change appearance because the firmware learned it could be
/// rearranged. Same six readings, same order, same emphasis — the difference
/// is only that they are now a list somebody can edit.
static inline void kdSlotsDefault(KindleSlotList& list,
                                  const char* outdoorId, const char* indoorId) {
    list.clear();
    if (!outdoorId || !*outdoorId) outdoorId = "outdoor";
    if (!indoorId  || !*indoorId)  indoorId  = "indoor";

    list.add(outdoorId, "temperature", KD_T("OUTSIDE", "НАВЪН"), KSLOT_HERO,
             KSLOT_DECIMALS_AUTO, KSLOTF_BOLD | KSLOTF_UNIT | KSLOTF_AGE);
    list.add(outdoorId, "humidity",    nullptr, KSLOT_MEDIUM);
    list.add(outdoorId, "pressure",    nullptr, KSLOT_MEDIUM,
             KSLOT_DECIMALS_AUTO, KSLOTF_UNIT | KSLOTF_TREND);

    list.add(indoorId,  "temperature", KD_T("INSIDE", "ВЪТРЕ"), KSLOT_LARGE,
             KSLOT_DECIMALS_AUTO, KSLOTF_UNIT | KSLOTF_AGE);
    list.add(indoorId,  "humidity",    nullptr, KSLOT_MEDIUM);
    list.add(indoorId,  "aqi",         nullptr, KSLOT_MEDIUM);
}
