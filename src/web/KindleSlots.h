// ============================================================================
// src/web/KindleSlots.h — what the e-ink dashboard shows, and where
//
// The page began as six hardwired readings: the outdoor temperature, humidity
// and pressure, and the indoor temperature, humidity and AQI. That is a fine
// dashboard for exactly one hardware configuration, and this firmware has
// twenty sensor plugins between them producing twenty-nine distinct metrics.
// The cost was not only the metrics it could not show — it was the ones it
// insisted on: a BMP280 outdoors measures no humidity, so the humidity row
// rendered a dash forever in a space nothing could use.
//
// The fix went too far the other way for a while. The page became a free list
// of readings that packed themselves into rows, which could show anything and
// therefore had no shape: the reader chose the order and the sizes, and got a
// different page every time a sensor went quiet.
//
// SO: NAMED PLACES, NOT A FLOW. The layout is fixed and the CONTENT of each
// place is the reader's. There are eleven of them and they are always in the
// same spot, at the same size, whether or not anything is configured into them:
//
//     ┌────────────────────────────────┬──────────────────────────────┐
//     │ «outdoor group label»          │            17:40             │
//     │                                │ ──────────────────────────── │
//     │   HERO / BIG                   │ «indoor group label»         │
//     │   24 h low-to-high · age       │           IN2      IN3       │
//     │                                │  IN1      44%       42       │
//     │                                │                              │
//     │   G1     G2     G3             │                              │
//     │   G4     G5     G6             │                              │
//     └────────────────────────────────┴──────────────────────────────┘
//
// HERO and BIG share one baseline with a slash between them, under a single
// group label, because they are usually one measurement of one parcel of air —
// `8.4° / 71%` — and a line break between those two puts a paragraph boundary
// through a single reading. The line under them is the 24-hour low-to-high of
// the HERO's own metric plus how old the reading is.
//
// G1..G6 are a table of up to three across and two deep, caption above value,
// and every row divides its own width by however many cells it ended up with —
// two readings are two halves, not two of three thirds with the last one white.
// IN1..IN3 are one row under the clock; IN1 is set much larger and carries no
// caption, because the group heading directly above it already names the room,
// and the other two sit on its bottom edge.
//
// AN EMPTY PLACE IS SKIPPED AND THE ONES AFTER IT CLOSE UP. That is the BMP280
// case and the reason any of this exists: no humidity reading, no humidity in
// the grid, no hole where one used to be. It is also how "three fields, or two"
// works for the indoor row — leave IN3 unconfigured and the other two spread.
//
// WHAT A PLACE DOES NOT CARRY IS A SIZE OR A COORDINATE. The dashboard is
// rendered at 600x800 on a Kindle 7 and at 1072x1448 on a Paperwhite, by two
// entirely different renderers — a CSS page and an FBInk shell script. Pixel
// positions chosen for one are wrong for the other, and the type scale was
// measured on the panel and is not the reader's to get wrong. The place decides
// how big its number is; the reader decides which number it is.
//
// NO ARDUINO IN HERE, for the reason NodeTable.h has none: the defaults, the
// label rules and the closing-up are the parts most likely to be wrong, and
// tests/host/test_kindle_slots.cpp can exercise all of them on the build host.
// Loading and saving needs ArduinoJson and a filesystem, so that lives in
// KindleSlotStore.cpp.
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
// The eleven places
// ---------------------------------------------------------------------------
// The order is the drawing order and the storage order. Appending is safe;
// renumbering is not — but nothing depends on the numbers anyway, because every
// place is addressed by its KEY in the file, the API and the shell renderer.
enum KindleZone : uint8_t {
    KZ_HERO = 0,   ///< the glance value, largest type on the page
    KZ_BIG,        ///< beside it, after a slash, about half its size
    KZ_G1,         ///< the grid, in reading order: up to three across, two deep
    KZ_G2,
    KZ_G3,
    KZ_G4,
    KZ_G5,
    KZ_G6,
    KZ_IN1,        ///< the indoor row under the clock; IN1 is the large one
    KZ_IN2,
    KZ_IN3,
    KZ_COUNT
};

/// How many of the grid and indoor places there are, named rather than spelled
/// as 6 and 3 at each of the dozen sites that walk them.
static const int KZ_GRID_COUNT   = 6;
static const int KZ_INDOOR_COUNT = 3;

/// The widest the grid gets. Two rows of three is what fits above the chart on
/// a 600x800 panel; a third row would run into it.
static const int KZ_GRID_COLS = 3;

/// The stable key each place is stored and addressed by — in the JSON file, in
/// the API, and in the shell renderer's variable names. A NUMBER would have
/// been shorter and would also mean that inserting a place one day silently
/// moved everybody's configuration one place along.
static inline const char* kdZoneKey(uint8_t z) {
    switch (z) {
        case KZ_HERO: return "hero";
        case KZ_BIG:  return "big";
        case KZ_G1:   return "g1";
        case KZ_G2:   return "g2";
        case KZ_G3:   return "g3";
        case KZ_G4:   return "g4";
        case KZ_G5:   return "g5";
        case KZ_G6:   return "g6";
        case KZ_IN1:  return "in1";
        case KZ_IN2:  return "in2";
        case KZ_IN3:  return "in3";
        default:      return "";
    }
}

/// The reverse, for reading a file or a form. KZ_COUNT means "no such place",
/// which is a value the caller must check — a key that does not resolve is a
/// field from a future build, not something to guess at.
static inline uint8_t kdZoneFromKey(const char* key) {
    if (!key || !*key) return KZ_COUNT;
    for (uint8_t z = 0; z < KZ_COUNT; z++)
        if (strcmp(kdZoneKey(z), key) == 0) return z;
    return KZ_COUNT;
}

/// Which of the two groups a place belongs to. The group is what carries the
/// heading above it and, on the shell renderer, which column it is drawn in.
static inline bool kdZoneIsIndoor(uint8_t z) { return z >= KZ_IN1 && z <= KZ_IN3; }
static inline bool kdZoneIsGrid(uint8_t z)   { return z >= KZ_G1  && z <= KZ_G6;  }

// ---------------------------------------------------------------------------
// Per-place options
// ---------------------------------------------------------------------------
constexpr uint8_t KSLOTF_BOLD     = 0x01;  ///< draw the value bold
constexpr uint8_t KSLOTF_UNIT     = 0x02;  ///< append the reading's own unit
constexpr uint8_t KSLOTF_AGE      = 0x04;  ///< append "· 4m" when it is stale
constexpr uint8_t KSLOTF_TREND    = 0x08;  ///< append the 3 h tendency arrow
constexpr uint8_t KSLOTF_ALL      = 0x0F;

/// 0xFF in `decimals` means "use the metric's own convention".
static const uint8_t KSLOT_DECIMALS_AUTO = 0xFF;

// ---------------------------------------------------------------------------
// How dark a value is drawn
// ---------------------------------------------------------------------------
// FOUR LEVELS, NOT A COLOUR PICKER. The page's palette is #000 #444 #777 #aaa
// plus two washes, and it is four values rather than a gradient because the
// panel has sixteen real grey levels and the dithering worth avoiding comes
// from tones too close together. Spaced this far apart each renders solid; a
// free choice would mostly offer a way to pick two that mush into each other.
//
// This is per PLACE, so a reader who wants the pressure to recede behind the
// temperature can say so without the firmware having an opinion about which
// readings matter — which was the whole reason the layout became configurable.
enum KindleInk : uint8_t {
    KINK_BLACK = 0,   ///< #000 — the default, and what a headline wants
    KINK_DARK  = 1,   ///< #444
    KINK_MID   = 2,   ///< #777
    KINK_LIGHT = 3,   ///< #aaa — legible on the panel, but only just
    KINK_COUNT
};

/// The CSS colour for a level. Also the order the settings form lists them in.
static inline const char* kdInkCss(uint8_t ink) {
    switch (ink) {
        case KINK_DARK:  return "#444";
        case KINK_MID:   return "#777";
        case KINK_LIGHT: return "#aaa";
        default:         return "#000";
    }
}

/// What FBInk calls the same level. The shell renderer takes a colour NAME, and
/// these four are the ones the rest of update_dash.sh already uses.
static inline const char* kdInkFbink(uint8_t ink) {
    switch (ink) {
        case KINK_DARK:  return "GRAY4";
        case KINK_MID:   return "GRAY7";
        case KINK_LIGHT: return "GRAY10";
        default:         return "BLACK";
    }
}

// ---------------------------------------------------------------------------
// One place's contents
// ---------------------------------------------------------------------------
// The id and metric buffers match SensorReading's, so a name that fits the
// pipeline fits here — and one that does not was already being truncated
// upstream rather than by this.
struct KindleSlot {
    char    sensorId[17];   ///< "" = nothing configured here; the place is skipped
    char    metric[17];
    char    label[17];      ///< "" = derive from the metric
    uint8_t decimals;       ///< KSLOT_DECIMALS_AUTO or 0..3
    uint8_t flags;          ///< KSLOTF_*
    uint8_t ink;            ///< KindleInk — how dark the value is drawn

    bool used() const { return sensorId[0] != '\0' && metric[0] != '\0'; }
};

// ---------------------------------------------------------------------------
// What a metric is called, and how precisely it is worth showing
// ---------------------------------------------------------------------------
// A table rather than per-place configuration, because "pm25" should read as
// "PM2.5" on every dashboard ever configured and nobody should have to type
// that. The label is still overridable — a place named "Спалня" beats any
// table — but the default is right often enough that most places need nothing.
struct KdMetricStyle {
    const char* metric;
    const char* label;
    uint8_t     decimals;
    /// What to print after the value, when the reading's own unit is not what
    /// a glanceable page wants. nullptr means "use the sensor's".
    ///
    /// The pipeline's units are chosen to be unambiguous between machines:
    /// temperature is "C", particulates are "ug/m3". On a page read from
    /// across a room the temperature wants the degree sign it has always had —
    /// dropping it to "8.4 C" would be a visible regression from the design
    /// this replaced — and "µg/m³" is what the number means to a person.
    const char* unit;
};

static const KdMetricStyle KD_METRIC_STYLE[] = {
    // metric            label                    dec  display unit
    { "temperature",   KD_T("TEMP",  "ТЕМП"),    1,   "°"   },
    { "humidity",      KD_T("HUM",   "ВЛАГА"),   0,   "%"        },
    { "humidity_amb",  KD_T("HUM",   "ВЛАГА"),   0,   "%"        },
    { "dew_point",     KD_T("DEW",   "РОСА"),    1,   "°"   },
    { "pressure",      KD_T("PRESS", "НАЛЯГ"),   0,   nullptr    },  // re-united elsewhere
    { "aqi",           "AQI",                    0,   ""         },
    { "co2",           "CO₂",               0,   "ppm"      },
    { "eco2",          "eCO₂",              0,   "ppm"      },
    { "tvoc",          "TVOC",                   0,   "ppb"      },
    { "pm1",           "PM1",                    0,   "µg/m³" },
    { "pm25",          "PM2.5",                  0,   "µg/m³" },
    { "pm4",           "PM4",                    0,   "µg/m³" },
    { "pm10",          "PM10",                   0,   "µg/m³" },
    { "lux",           KD_T("LIGHT", "СВЕТЛ"),   0,   "lx"       },
    { "uva",           "UVA",                    1,   nullptr    },
    { "uvb",           "UVB",                    1,   nullptr    },
    { "rain",          KD_T("RAIN",  "ДЪЖД"),    1,   "mm"       },
    { "rain_rate",     KD_T("RAIN/h","ДЪЖД/ч"),  1,   "mm/h"     },
    { "rain_total",    KD_T("RAIN Σ","ДЪЖД Σ"), 1, "mm" },
    { "wind",          KD_T("WIND",  "ВЯТЪР"),   1,   nullptr    },
    { "wind_speed",    KD_T("WIND",  "ВЯТЪР"),   1,   nullptr    },
    { "wind_direction",KD_T("DIR",   "ПОСОКА"),  0,   "°"   },
    { "soil_moisture", KD_T("SOIL",  "ПОЧВА"),   0,   "%"        },
    { "flow_rate",     KD_T("FLOW",  "ДЕБИТ"),   1,   nullptr    },
    { "battery_voltage", KD_T("BATT","БАТЕРИЯ"), 2,   "V"        },
    { "battery_percent", KD_T("BATT","БАТЕРИЯ"), 0,   "%"        },
    { "battery_days",  KD_T("DAYS",  "ДНИ"),     0,   "d"        },
};
static const int KD_METRIC_STYLE_COUNT =
    (int)(sizeof(KD_METRIC_STYLE) / sizeof(KD_METRIC_STYLE[0]));

static inline const KdMetricStyle* kdMetricStyle(const char* metric) {
    if (!metric || !*metric) return nullptr;
    for (int i = 0; i < KD_METRIC_STYLE_COUNT; i++)
        if (strcmp(KD_METRIC_STYLE[i].metric, metric) == 0) return &KD_METRIC_STYLE[i];
    return nullptr;
}

/// The label to draw: the place's own if set, else the metric's, else the
/// metric name itself — which is not pretty for something unlisted, but it is
/// honest, and a table that quietly rendered nothing would be worse.
static inline const char* kdSlotLabel(const KindleSlot& s) {
    if (s.label[0]) return s.label;
    const KdMetricStyle* st = kdMetricStyle(s.metric);
    return st ? st->label : s.metric;
}

/// What to print after the value. The table's choice when it has one, else the
/// unit the sensor reported, else nothing.
static inline const char* kdSlotUnit(const KindleSlot& s, const char* readingUnit) {
    const KdMetricStyle* st = kdMetricStyle(s.metric);
    if (st && st->unit) return st->unit;
    return readingUnit ? readingUnit : "";
}

static inline uint8_t kdSlotDecimals(const KindleSlot& s) {
    if (s.decimals != KSLOT_DECIMALS_AUTO) return s.decimals > 3 ? 3 : s.decimals;
    const KdMetricStyle* st = kdMetricStyle(s.metric);
    return st ? st->decimals : 1;
}

// ---------------------------------------------------------------------------
// The whole configuration
// ---------------------------------------------------------------------------
/// The eleven places plus the two group headings above them.
///
/// The headings are stored rather than compiled in because they are the one
/// piece of text on this page that is about the reader's house and not about
/// the data: "НАВЪН" and "ВЪТРЕ" are right for most people, "Балкон" and
/// "Спалня" are right for the person who put the sensors there, and neither the
/// firmware nor the metric table can know which.
struct KindleZones {
    KindleSlot z[KZ_COUNT];
    char       groupOut[17];   ///< "" = the built-in heading
    char       groupIn[17];

    void clear() {
        for (int i = 0; i < KZ_COUNT; i++) z[i] = KindleSlot{};
        groupOut[0] = '\0';
        groupIn[0]  = '\0';
    }

    bool set(uint8_t zone, const char* sensorId, const char* metric,
             const char* label = nullptr,
             uint8_t decimals = KSLOT_DECIMALS_AUTO,
             uint8_t flags = KSLOTF_UNIT,
             uint8_t ink = KINK_BLACK) {
        if (zone >= KZ_COUNT || !sensorId || !metric) return false;
        KindleSlot& s = z[zone];
        s = KindleSlot{};
        strncpy(s.sensorId, sensorId, sizeof(s.sensorId) - 1);
        strncpy(s.metric,   metric,   sizeof(s.metric)   - 1);
        if (label) strncpy(s.label, label, sizeof(s.label) - 1);
        s.decimals = decimals;
        s.flags    = flags;
        s.ink      = ink;
        if (!s.used()) { s = KindleSlot{}; return false; }
        return true;
    }

    /// How many places have something configured in them. Not the same as how
    /// many will be DRAWN — that depends on which sensors are reporting.
    int configured() const {
        int n = 0;
        for (int i = 0; i < KZ_COUNT; i++) if (z[i].used()) n++;
        return n;
    }
};

/// The heading above the outdoor block, and above the indoor one.
static inline const char* kdGroupOutLabel(const KindleZones& k) {
    return k.groupOut[0] ? k.groupOut : KD_T("OUTSIDE", "НАВЪН");
}
static inline const char* kdGroupInLabel(const KindleZones& k) {
    return k.groupIn[0] ? k.groupIn : KD_T("INSIDE", "ВЪТРЕ");
}

/// Bring anything that arrived from a file or a form into range.
///
/// Applied on the way IN, not at the point of use, for the reason kdSkinClamp()
/// exists: a renderer should never have to wonder whether the byte it was
/// handed is one of the values it knows about.
static inline void kdZonesClamp(KindleZones& k) {
    for (int i = 0; i < KZ_COUNT; i++) {
        KindleSlot& s = k.z[i];
        s.sensorId[sizeof(s.sensorId) - 1] = '\0';
        s.metric[sizeof(s.metric) - 1]     = '\0';
        s.label[sizeof(s.label) - 1]       = '\0';
        if (!s.used()) { s = KindleSlot{}; continue; }   // half-filled is empty
        if (s.decimals != KSLOT_DECIMALS_AUTO && s.decimals > 3) s.decimals = 3;
        if (s.ink >= KINK_COUNT) s.ink = KINK_BLACK;
        s.flags &= KSLOTF_ALL;
    }
    k.groupOut[sizeof(k.groupOut) - 1] = '\0';
    k.groupIn[sizeof(k.groupIn)   - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Closing up behind an empty place
// ---------------------------------------------------------------------------
/// Collect the places of one group that will actually be drawn, in order.
///
/// `visible[KZ_COUNT]` says which places have a reading to show; pass nullptr to
/// treat everything configured as visible, which is what the settings preview
/// wants. Writes zone indices into `out` and returns how many.
///
/// THE ORDER IS NEVER CHANGED. A best-fit arrangement would use the space
/// better and would also mean the reader's chosen order rearranging itself
/// whenever a sensor went quiet, which on a glanceable display is worse than a
/// gap. What happens instead is that the survivors move UP into the places in
/// front of them: three configured grid cells with the second one quiet are
/// drawn top-left, top-right, bottom-left.
static inline int kdZonesUsed(const KindleZones& k, const bool* visible,
                              uint8_t first, int count, uint8_t* out) {
    if (!out || count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t z = (uint8_t)(first + i);
        if (z >= KZ_COUNT) break;
        if (!k.z[z].used()) continue;
        if (visible && !visible[z]) continue;
        out[n++] = z;
    }
    return n;
}

/// The grid's survivors, in reading order. At most KZ_GRID_COUNT.
static inline int kdGridUsed(const KindleZones& k, const bool* visible, uint8_t* out) {
    return kdZonesUsed(k, visible, KZ_G1, KZ_GRID_COUNT, out);
}

/// How to break `n` grid cells into rows, and how many go on each.
///
/// THE ROWS ARE BALANCED AND ALWAYS FULL. Three across is the widest the grid
/// gets, but filling greedily from the left would put four cells out as three
/// and one — and a lone cell taking a whole row, or taking a third of one and
/// leaving two thirds white, are both worse than two rows of two. So the number
/// of rows comes from the cap and the cells are then spread as evenly as they
/// go, remainder to the earlier rows:
///
///     1 -> 1        4 -> 2 2
///     2 -> 2        5 -> 3 2
///     3 -> 3        6 -> 3 3
///
/// Each row then divides its own width by its own count, which is what makes
/// "no empty space" true whichever places the reader filled in.
///
/// Writes the per-row counts into `rows` (at most 2 for a six-place grid) and
/// returns how many rows there are.
static inline int kdGridRowSplit(int n, int* rows, int maxRows) {
    if (!rows || maxRows <= 0 || n <= 0) return 0;
    int nRows = (n + KZ_GRID_COLS - 1) / KZ_GRID_COLS;
    if (nRows > maxRows) nRows = maxRows;

    const int base = n / nRows;
    int extra = n - base * nRows;          // 0 .. nRows-1
    for (int r = 0; r < nRows; r++) {
        rows[r] = base + (extra > 0 ? 1 : 0);
        if (extra > 0) extra--;
    }
    return nRows;
}

/// The indoor row's survivors. At most KZ_INDOOR_COUNT — and this is what makes
/// "three fields, or two" a setting rather than a mode: leave IN3 empty and two
/// come back.
static inline int kdIndoorUsed(const KindleZones& k, const bool* visible, uint8_t* out) {
    return kdZonesUsed(k, visible, KZ_IN1, KZ_INDOOR_COUNT, out);
}

// ---------------------------------------------------------------------------
// The default configuration
// ---------------------------------------------------------------------------
/// WHAT THE DASHBOARD DREW BEFORE ANY OF THIS EXISTED, in the places that now
/// hold it. A device upgrading into this has been looking at the same page for
/// months and should not find it rearranged because the firmware learned it
/// could be: outdoor temperature and humidity on the headline, pressure and dew
/// point in the grid, the indoor three under the clock.
///
/// The grid's third and fourth places are left EMPTY rather than filled with
/// something plausible. A dashboard that arrives showing a metric the hardware
/// does not have would be showing a dash, which is the fault this whole design
/// was built to remove; an empty place is an invitation to put something in it.
static inline void kdZonesDefault(KindleZones& k,
                                  const char* outdoorId, const char* indoorId) {
    k.clear();
    if (!outdoorId || !*outdoorId) outdoorId = "outdoor";
    if (!indoorId  || !*indoorId)  indoorId  = "indoor";

    k.set(KZ_HERO, outdoorId, "temperature", nullptr,
          KSLOT_DECIMALS_AUTO, KSLOTF_BOLD | KSLOTF_UNIT | KSLOTF_AGE);
    k.set(KZ_BIG,  outdoorId, "humidity");

    k.set(KZ_G1,   outdoorId, "pressure",  nullptr,
          KSLOT_DECIMALS_AUTO, KSLOTF_UNIT | KSLOTF_TREND);
    k.set(KZ_G2,   outdoorId, "dew_point");

    k.set(KZ_IN1,  indoorId,  "temperature", nullptr,
          KSLOT_DECIMALS_AUTO, KSLOTF_UNIT | KSLOTF_AGE);
    k.set(KZ_IN2,  indoorId,  "humidity");
    k.set(KZ_IN3,  indoorId,  "aqi");
}
