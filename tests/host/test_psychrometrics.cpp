// Host unit tests for src/utils/Psychrometrics.h
//   - Magnus saturation vapour pressure against published reference values
//   - dewPointC() round-trips and known-good pairs
//   - rhAtTempC() self-heating correction, including the invariance property
//     the BME688 humidity correction depends on
//   - out-of-range / degenerate inputs return NAN rather than garbage
#include "src/utils/Psychrometrics.h"
#include "check.h"

using namespace Psychro;

// Absolute tolerance helper — the Magnus fit is an approximation, so the
// reference comparisons below are checked to a stated tolerance rather than
// bit-exactly.
static bool near(float a, float b, float tol) {
    return isfinite(a) && isfinite(b) && fabsf(a - b) <= tol;
}

// Relative comparison, for the saturation-pressure reference points: the
// Magnus fit's error is specified as a percentage of the value, so an
// absolute tolerance would be far too loose at -15 °C and too tight at 30 °C.
static bool nearPct(float actual, float expected, float pct) {
    return isfinite(actual) && fabsf(actual - expected) <= fabsf(expected) * pct / 100.0f;
}

static void test_saturation_pressure_reference() {
    // Reference saturation vapour pressures over water (WMO / Smithsonian
    // tables), hPa. The Magnus form with the WMO 2008 coefficients is
    // specified to better than 0.3 % over -40..+50 °C; these points sit at
    // ~0.28 % at the top of the range, so 0.3 % is the honest bound. A tighter
    // bound would be asserting more precision than the approximation has.
    CHECK(nearPct(saturationVaporPressureHpa(0.0f),   6.112f, 0.3f));
    CHECK(nearPct(saturationVaporPressureHpa(10.0f),  12.28f, 0.3f));
    CHECK(nearPct(saturationVaporPressureHpa(20.0f),  23.39f, 0.3f));
    CHECK(nearPct(saturationVaporPressureHpa(30.0f),  42.46f, 0.3f));

    // Sub-zero: the over-water branch is used deliberately (dew point, not
    // frost point). Goff-Gratch over supercooled water gives 1.912 hPa at
    // -15 °C; Magnus returns 1.919, a 0.35 % overshoot. The looser bound here
    // is not slack — below freezing the "over water" reference is itself an
    // extrapolation and the published formulations disagree by a similar
    // margin. In dew-point terms 0.35 % of vapour pressure is about 0.05 °C.
    CHECK(nearPct(saturationVaporPressureHpa(-15.0f), 1.912f, 0.5f));

    // Monotonic increasing across the working range.
    float prev = saturationVaporPressureHpa(-50.0f);
    for (float t = -49.0f; t <= 70.0f; t += 1.0f) {
        float cur = saturationVaporPressureHpa(t);
        CHECK(cur > prev);
        prev = cur;
    }
}

static void test_dew_point_known_values() {
    // 100 % RH: dew point equals the air temperature.
    CHECK(near(dewPointC(20.0f, 100.0f), 20.0f, 0.05f));
    CHECK(near(dewPointC(-5.0f, 100.0f), -5.0f, 0.05f));

    // Textbook pair: 20 °C / 50 % RH -> ~9.3 °C dew point.
    CHECK(near(dewPointC(20.0f, 50.0f), 9.3f, 0.2f));

    // 25 °C / 60 % RH -> ~16.7 °C.
    CHECK(near(dewPointC(25.0f, 60.0f), 16.7f, 0.2f));

    // Dew point is always <= air temperature.
    for (float t = -20.0f; t <= 50.0f; t += 5.0f) {
        for (float rh = 1.0f; rh <= 100.0f; rh += 9.0f) {
            float td = dewPointC(t, rh);
            CHECK(isfinite(td));
            CHECK(td <= t + 0.01f);
        }
    }
}

static void test_dew_point_round_trip() {
    // rhAtTempC(dewPointC(T, RH), T) must recover RH — this is the identity
    // the whole correction rests on.
    for (float t = -20.0f; t <= 45.0f; t += 5.0f) {
        for (float rh = 5.0f; rh <= 100.0f; rh += 5.0f) {
            float td  = dewPointC(t, rh);
            float rh2 = rhAtTempC(td, t);
            CHECK(near(rh2, rh, 0.5f));
        }
    }
}

static void test_self_heating_correction() {
    // The BME688 case: the die runs 2 °C above ambient, so the element sees
    // 22 °C where the air is 20 °C. Air at 20 °C / 50 % RH has a dew point of
    // ~9.3 °C; at 22 °C that same dew point reads as a LOWER RH.
    const float trueAirC   = 20.0f;
    const float trueRh     = 50.0f;
    const float dieC       = 22.0f;

    const float dewTrue    = dewPointC(trueAirC, trueRh);
    const float rhAtDie    = rhAtTempC(dewTrue, dieC);

    // Reported RH must be lower than the true RH...
    CHECK(rhAtDie < trueRh);
    // ...by roughly 6 % relative per °C, i.e. ~12 % relative over 2 °C.
    // 50 % * (1 - 0.12) ~= 44 %.
    CHECK(near(rhAtDie, 44.0f, 1.5f));

    // Now the correction the driver performs: dew point recovered from the
    // die-temperature pair, re-expressed at the true air temperature, must
    // return the original ambient RH.
    const float dewFromDie = dewPointC(dieC, rhAtDie);
    CHECK(near(dewFromDie, dewTrue, 0.05f));
    CHECK(near(rhAtTempC(dewFromDie, trueAirC), trueRh, 0.5f));
}

static void test_heated_enclosure_case() {
    // Frost-protection scenario from the enclosure heater: -15 °C ambient at
    // 100 % RH, enclosure held at +2 °C. A sensor inside the enclosure sees a
    // drastically lower RH even though the air's water content is unchanged.
    const float dew = dewPointC(-15.0f, 100.0f);
    CHECK(near(dew, -15.0f, 0.05f));

    const float rhInside = rhAtTempC(dew, 2.0f);
    // es(-15)/es(2) ~= 1.91/7.06 ~= 27 %.
    CHECK(rhInside < 35.0f);
    CHECK(rhInside > 20.0f);

    // And the correction recovers saturation at the true ambient temperature.
    CHECK(near(rhAtTempC(dew, -15.0f), 100.0f, 0.5f));
}

static void test_clamping_and_invalid_inputs() {
    // RH is clamped to 100 on the way in: a sensor reporting 105 % must not
    // produce a dew point above the air temperature.
    CHECK(near(dewPointC(10.0f, 105.0f), 10.0f, 0.05f));

    // RH floored at 0.1 % — must stay finite rather than going to -inf.
    float dryDew = dewPointC(20.0f, 0.0f);
    CHECK(isfinite(dryDew));
    CHECK(dryDew < -40.0f);

    // Out-of-range temperatures return NAN, not a plausible-looking number.
    CHECK(!isfinite(dewPointC(-999.0f, 50.0f)));
    CHECK(!isfinite(dewPointC(999.0f, 50.0f)));
    CHECK(!isfinite(saturationVaporPressureHpa(200.0f)));
    CHECK(!isfinite(rhAtTempC(10.0f, -999.0f)));
    CHECK(!isfinite(rhAtTempC(-999.0f, 10.0f)));

    // NAN in, NAN out.
    CHECK(!isfinite(dewPointC(NAN, 50.0f)));
    CHECK(!isfinite(dewPointC(20.0f, NAN)));

    // isValidTemp() boundaries.
    CHECK(isValidTemp(0.0f));
    CHECK(isValidTemp(-59.0f));
    CHECK(isValidTemp(79.0f));
    CHECK(!isValidTemp(-61.0f));
    CHECK(!isValidTemp(81.0f));
    CHECK(!isValidTemp(NAN));
}

static void test_rh_never_exceeds_100() {
    // Measurement noise can put the reference temperature slightly BELOW the
    // dew point (probe error, or genuine supersaturation at the sensor). The
    // result must clamp at 100 rather than exceeding it — ProcessingTask's
    // isPlausible() rejects humidity > 100 and would drop the reading.
    float dew = 10.0f;
    CHECK(near(rhAtTempC(dew, 9.0f),  100.0f, 0.001f));
    CHECK(near(rhAtTempC(dew, 5.0f),  100.0f, 0.001f));
    CHECK(rhAtTempC(dew, 10.0f) <= 100.0f);
}

int main() {
    RUN(test_saturation_pressure_reference);
    RUN(test_dew_point_known_values);
    RUN(test_dew_point_round_trip);
    RUN(test_self_heating_correction);
    RUN(test_heated_enclosure_case);
    RUN(test_clamping_and_invalid_inputs);
    RUN(test_rh_never_exceeds_100);
    return SUMMARY();
}
