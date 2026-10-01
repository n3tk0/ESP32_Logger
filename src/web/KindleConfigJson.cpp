// src/web/KindleConfigJson.cpp — see KindleConfigJson.h.
#include "KindleConfigJson.h"
#include "KindleSkin.h"          // kdSkinClamp and the rotation/week/rule/size helpers

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef FEATURE_KINDLE_DASHBOARD
#include "KindleDashboard.h"     // KINDLE_* defaults the API shows
#endif

namespace {

enum : uint8_t { KF_U8, KF_U16, KF_STR };

// A plain field: one stored value, the same meaning under both names.
struct KdField {
    const char* api;     // /api/kindle/config
    const char* file;    // the settings file
    uint16_t    off;     // offsetof(KindleConfig, …)
    uint8_t     type;
    uint8_t     size;    // bytes, for KF_STR the buffer including the NUL
};

#define KF(api, file, member, type) \
    { api, file, (uint16_t)offsetof(KindleConfig, member), type, \
      (uint8_t)sizeof(((KindleConfig*)nullptr)->member) }

// Adding a plain field to KindleConfig means one line here, and it is then
// shown, posted, backed up and restored.
const KdField KD_FIELDS[] = {
    KF("face",              "face",            face,            KF_U8),
    KF("face_custom",       "faceCustom",      faceCustom,      KF_STR),
    KF("bold",              "boldZones",       boldZones,       KF_U16),
    KF("show",              "showFlags",       showFlags,       KF_U16),
    KF("clock_style",       "clockStyle",      clockStyle,      KF_U8),
    KF("time_format",       "timeFormat",      timeFormat,      KF_U8),
    KF("date_format",       "dateFormat",      dateFormat,      KF_U8),
    KF("pressure_unit",     "pressureUnit",    pressureUnit,    KF_U8),
    KF("decimals",          "tempDecimals",    tempDecimals,    KF_U8),
    KF("refresh_sec",       "refreshSec",      refreshSec,      KF_U16),
    KF("follow_data",       "followData",      followData,      KF_U8),
    KF("clock_pin_refresh", "clockPinRefresh", clockPinRefresh, KF_U8),
    KF("fbink_res_w",       "fbinkResW",       fbinkResW,       KF_U16),
    KF("outdoor_sensor",    "outdoorSensor",   outdoorSensor,   KF_STR),
    KF("indoor_sensor",     "indoorSensor",    indoorSensor,    KF_STR),
    KF("lang",              "lang",            lang,            KF_U8),
    KF("layout_mode",       "layoutMode",      layoutMode,      KF_U8),
    KF("browser_dev",       "browserDev",      browserDev,      KF_U8),
    KF("browser_bar",       "browserBar",      browserBar,      KF_U8),
};
#undef KF

inline uint8_t* at(KindleConfig& k, const KdField& f) {
    return reinterpret_cast<uint8_t*>(&k) + f.off;
}
inline const uint8_t* at(const KindleConfig& k, const KdField& f) {
    return reinterpret_cast<const uint8_t*>(&k) + f.off;
}

long getNum(const KindleConfig& k, const KdField& f) {
    if (f.type == KF_U16) { uint16_t v; memcpy(&v, at(k, f), 2); return v; }
    return *at(k, f);
}

// Out-of-range numbers are held to the field's width rather than wrapped;
// kdSkinClamp() then narrows each to what it can mean.
void setNum(KindleConfig& k, const KdField& f, long v) {
    const long hi = (f.type == KF_U16) ? 0xFFFF : 0xFF;
    if (v < 0) v = 0;
    if (v > hi) v = hi;
    if (f.type == KF_U16) { const uint16_t u = (uint16_t)v; memcpy(at(k, f), &u, 2); }
    else                  *at(k, f) = (uint8_t)v;
}

void setStr(KindleConfig& k, const KdField& f, const char* v) {
    char* dst = reinterpret_cast<char*>(at(k, f));
    strncpy(dst, v, f.size - 1);
    dst[f.size - 1] = '\0';
}

void fieldsToJson(const KindleConfig& k, JsonObject out, bool api) {
    for (const KdField& f : KD_FIELDS) {
        const char* key = api ? f.api : f.file;
        if (f.type == KF_STR) out[key] = reinterpret_cast<const char*>(at(k, f));
        else                  out[key] = getNum(k, f);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The settings file
// ---------------------------------------------------------------------------
void kdConfigToFile(const KindleConfig& k, JsonObject out) {
    fieldsToJson(k, out, false);
    out["rotation"]     = k.rotation * 90;        // degrees, as the API and ?rot= spell it
    out["pageRotation"] = kdPageRotDeg(k);        // -1: the same as the panel
    out["clockOff"]     = k.clockOff;
    out["clockSync"]    = kdClockSyncDays(k);     // days, 0 never
    // The week strip's look and content, and the rules, as the bytes they
    // are stored in — kdSkinClamp() narrows them on the way back in.
    out["weekStyle"]    = k.weekStyle;
    out["rules"]        = k.rules;
    out["metricSize"]   = k.metricSize;
}

void kdConfigFromFile(KindleConfig& k, JsonObjectConst in) {
    // Absent keys leave the field alone, so a settings file written by an
    // older firmware restores what it knew about and does not reset what it
    // never carried.
    for (const KdField& f : KD_FIELDS) {
        JsonVariantConst v = in[f.file];
        if (f.type == KF_STR) { if (v.is<const char*>()) setStr(k, f, v.as<const char*>()); }
        else if (v.is<long>())  setNum(k, f, v.as<long>());
    }
    if (k.fbinkResW > 4096) k.fbinkResW = 4096;

    if (in["rotation"].is<int>())
        k.rotation = kdRotFromDeg(in["rotation"].as<int>(), k.rotation);
    if (in["pageRotation"].is<int>())
        k.pageRot = kdPageRotFromDeg(in["pageRotation"].as<int>(), k.pageRot);
    if (in["clockOff"].is<int>())
        k.clockOff = (uint8_t)in["clockOff"].as<int>();
    if (in["clockSync"].is<int>())
        k.clockSync = kdClockSyncFromDays(in["clockSync"].as<int>(), k.clockSync);
    if (in["weekStyle"].is<int>())  k.weekStyle  = (uint8_t)in["weekStyle"].as<int>();
    if (in["rules"].is<int>())      k.rules      = (uint8_t)in["rules"].as<int>();
    if (in["metricSize"].is<int>()) k.metricSize = (uint8_t)in["metricSize"].as<int>();

    // An imported file is not a form: it can carry anything, including values
    // written by a firmware that had one more clock style than this one.
    // Clamped here so the renderer never has to consider that it might not
    // have been.
    kdSkinClamp(k);
}

// ---------------------------------------------------------------------------
// GET / POST /api/kindle/config
// ---------------------------------------------------------------------------
#ifdef FEATURE_KINDLE_DASHBOARD

void kdConfigToApi(const KindleConfig& k, JsonObject out) {
    // `lang` and `layout_mode` go out as stored, not resolved: the page's
    // selects have an "as built" / "auto" entry, and showing the decision it
    // leads to would turn that choice into a fixed one the next time somebody
    // pressed Save.
    fieldsToJson(k, out, true);
    // Refresh cadence and the sensors: 0 / 0xFF / empty mean "the built-in
    // default", and the page shows which one that is.
    out["refresh_sec"]       = k.refreshSec ? k.refreshSec : KINDLE_REFRESH_SEC;
    out["follow_data"]       = (k.followData == 0xFF) ? KINDLE_FOLLOW_DATA : (int)k.followData;
    out["clock_pin_refresh"] = (k.clockPinRefresh == 0xFF) ? KINDLE_CLOCK_PIN_REFRESH : (int)k.clockPinRefresh;
    out["outdoor_sensor"]    = (k.outdoorSensor[0] != '\0') ? k.outdoorSensor : KINDLE_OUTDOOR_SENSOR;
    out["indoor_sensor"]     = (k.indoorSensor[0] != '\0') ? k.indoorSensor : KINDLE_INDOOR_SENSOR;

    out["lang_built"]    = kdLangResolve(KLANG_AUTO);
    // The clock is a switch on the page like the chart and the week strip,
    // but kept out of `show` — whose bits a settings page older than this one
    // posts back without it, which would take the clock off on every Save.
    out["clock"]         = k.clockOff ? 0 : 1;
    // Degrees, which is what the reader's own dash.conf and ?rot= say too.
    out["rotation"]      = (int)k.rotation * 90;
    out["page_rotation"] = kdPageRotDeg(k);   // -1: the same as the panel
    out["clock_sync"]    = kdClockSyncDays(k);   // days; 0 never
    // The week strip: how its cells are drawn (0 filled .. 3 minimal), and
    // whether it holds the forecast instead of the calendar.
    out["week_style"]    = kdWeekStyle(k);
    out["week_forecast"] = kdWeekForecast(k) ? 1 : 0;
    // The dividing lines: weight 0..2 (1..3 px), ink 0..3 (light .. black),
    // style 0..2 (solid, dashed, dotted).
    out["rule_weight"]   = kdRuleWeight(k);
    out["rule_ink"]      = kdRuleInk(k);
    out["rule_style"]    = kdRuleStyle(k);
    // How large the readings are set, per cent of the most that fits.
    out["out_size"]      = kdOutSizePct(k);
    out["in_size"]       = kdInSizePct(k);
    // The width the page falls back to, read-only: a build-time constant, used
    // only when neither the page's address (?scr=) nor the Device setting
    // above says which reader it is for.
    out["page_w"]        = KINDLE_PAGE_W;
}

void kdConfigFromForm(KindleConfig& k, KdFormGet get, void* ctx) {
    for (const KdField& f : KD_FIELDS) {
        const char* v = get(ctx, f.api);
        if (!v) continue;
        if (f.type == KF_STR) setStr(k, f, v);
        else                  setNum(k, f, atol(v));
    }
    const char* v;
    if ((v = get(ctx, "clock")))         k.clockOff  = atol(v) ? 0 : 1;
    if ((v = get(ctx, "rotation")))      k.rotation  = kdRotFromDeg(atol(v), k.rotation);
    if ((v = get(ctx, "page_rotation"))) k.pageRot   = kdPageRotFromDeg(atol(v), k.pageRot);
    if ((v = get(ctx, "clock_sync")))    k.clockSync = kdClockSyncFromDays(atol(v), k.clockSync);

    if ((v = get(ctx, "week_style")))
        k.weekStyle = (uint8_t)((k.weekStyle & ~KWEEK_STYLE_MASK) | (atol(v) & KWEEK_STYLE_MASK));
    if ((v = get(ctx, "week_forecast"))) {
        if (atol(v)) k.weekStyle |= KWEEK_FORECAST;
        else         k.weekStyle &= (uint8_t)~KWEEK_FORECAST;
    }
    {
        int w = kdRuleWeight(k), ink = kdRuleInk(k), st = kdRuleStyle(k);
        if ((v = get(ctx, "rule_weight"))) w   = atol(v);
        if ((v = get(ctx, "rule_ink")))    ink = atol(v);
        if ((v = get(ctx, "rule_style")))  st  = atol(v);
        k.rules = kdRulesPack(w, ink, st);
    }
    {
        int o = k.metricSize & 0x0F, i = k.metricSize >> 4;
        if ((v = get(ctx, "out_size"))) o = kdSizeStepFromPct(atol(v));
        if ((v = get(ctx, "in_size")))  i = kdSizeStepFromPct(atol(v));
        k.metricSize = kdSizePack(o, i);
    }
}

#endif  // FEATURE_KINDLE_DASHBOARD
