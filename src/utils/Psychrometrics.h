// ============================================================================
// src/utils/Psychrometrics.h
//
// Magnus-Tetens humidity conversions, shared by BME688Sensor (dew point +
// self-heating-corrected RH) and HeaterModule (condensation-risk control).
//
// WHY THIS EXISTS
// ---------------
// A BME680/BME688 measures relative humidity at the temperature of its own
// die, which self-heats — from the MOX gas heater (320 °C micro-hotplate),
// from the board LDO, and from any enclosure heating around it.  RH is a
// ratio against the saturation vapour pressure at the sensing element's
// temperature, so a warm sensor reports a LOWER RH than the surrounding air
// actually has.  Near 20 °C the saturation pressure climbs ~7 %/°C, so a
// +2 °C self-heat turns a true 50 % RH into a reported ~44 %.
//
// The driver's use of the die temperature to compensate the humidity reading
// is correct — the element really is at that temperature.  What is wrong is
// interpreting the result as ambient RH.  Applying a `calibration.temperature
// .offset` does not fix it either: the offset is applied after the humidity
// compensation has already run (see BME688_Mini::performReading).
//
// The dew point is the way out.  It is a property of the air's absolute water
// content and is invariant under heating: warming a parcel of air changes its
// RH but not its dew point.  So:
//
//   1. dewPointC(t_die, rh_at_die)  → the TRUE ambient dew point.
//   2. rhAtTempC(dewPoint, t_air)   → the RH that dew point implies at the
//                                     real air temperature (e.g. a DS18B20
//                                     outside the heated zone).
//
// COEFFICIENTS
// ------------
// Magnus form with the WMO (2008) over-water coefficients:
//
//     es(T) = 6.112 hPa * exp( (A * T) / (B + T) ),  A = 17.62, B = 243.12 °C
//
// valid for -45 °C .. +60 °C over water.  Over-water coefficients are used
// across the whole range on purpose: below 0 °C this yields the *dew point*
// rather than the frost point, which is the meteorological convention and
// what weather feeds (sensor.community, openSenseMap) expect.
// ============================================================================
#pragma once

#include <math.h>

namespace Psychro {

// Magnus coefficients over water (WMO 2008).
constexpr float MAGNUS_A = 17.62f;
constexpr float MAGNUS_B = 243.12f;   // °C

// Physical bounds used to reject nonsense inputs.  Outside these the Magnus
// fit is not valid and the result would be silently wrong rather than
// obviously wrong, so callers get NAN instead.
constexpr float MIN_VALID_TEMP_C = -60.0f;
constexpr float MAX_VALID_TEMP_C = 80.0f;

/// True when `v` is finite and inside the range the Magnus fit covers.
inline bool isValidTemp(float tC) {
    return isfinite(tC) && tC > MIN_VALID_TEMP_C && tC < MAX_VALID_TEMP_C;
}

/// Saturation vapour pressure over water, in hPa. Returns NAN out of range.
inline float saturationVaporPressureHpa(float tC) {
    if (!isValidTemp(tC)) return NAN;
    return 6.112f * expf((MAGNUS_A * tC) / (MAGNUS_B + tC));
}

// ---------------------------------------------------------------------------
// dewPointC()
//   `tC`  — temperature of the humidity SENSING ELEMENT (the die), not the
//           calibrated/reported air temperature.  The measured RH is a ratio
//           against saturation at this temperature, so pairing the RH with
//           any other temperature produces a wrong dew point.
//   `rhPct` — relative humidity 0..100 as reported by the sensor.
//
//   Returns the dew point in °C, or NAN if either input is out of range.
//
//   RH is clamped to a 0.1 % floor before the log: a sensor reporting exactly
//   0 % (dead element, or a very dry heated enclosure) would otherwise yield
//   -inf and poison every downstream comparison.
// ---------------------------------------------------------------------------
inline float dewPointC(float tC, float rhPct) {
    if (!isValidTemp(tC))   return NAN;
    if (!isfinite(rhPct))   return NAN;
    if (rhPct > 100.0f) rhPct = 100.0f;
    if (rhPct < 0.1f)   rhPct = 0.1f;      // floor, see note above

    const float gamma = logf(rhPct / 100.0f) + (MAGNUS_A * tC) / (MAGNUS_B + tC);

    // gamma == MAGNUS_A would divide by zero; it corresponds to a dew point at
    // infinity and cannot occur for in-range inputs, but guard anyway.
    const float denom = MAGNUS_A - gamma;
    if (fabsf(denom) < 1e-6f) return NAN;

    const float td = (MAGNUS_B * gamma) / denom;
    return isfinite(td) ? td : NAN;
}

// ---------------------------------------------------------------------------
// rhAtTempC()
//   Re-expresses a dew point as a relative humidity at a different air
//   temperature.  This is the self-heating correction: feed it the true dew
//   point (from dewPointC above) and the real air temperature from a probe
//   that does not self-heat, and it returns the ambient RH.
//
//   Returns 0..100, or NAN if either input is out of range.  The result is
//   clamped to 100: when the air temperature is at or below the dew point the
//   air is saturated, and measurement noise in either input would otherwise
//   push the ratio slightly past 100 %.
// ---------------------------------------------------------------------------
inline float rhAtTempC(float dewPointCelsius, float airTempC) {
    if (!isValidTemp(dewPointCelsius)) return NAN;
    if (!isValidTemp(airTempC))        return NAN;

    const float esDew = saturationVaporPressureHpa(dewPointCelsius);
    const float esAir = saturationVaporPressureHpa(airTempC);
    if (!isfinite(esDew) || !isfinite(esAir) || esAir <= 0.0f) return NAN;

    float rh = 100.0f * (esDew / esAir);
    if (!isfinite(rh)) return NAN;
    if (rh > 100.0f) rh = 100.0f;
    if (rh < 0.0f)   rh = 0.0f;
    return rh;
}

}  // namespace Psychro
