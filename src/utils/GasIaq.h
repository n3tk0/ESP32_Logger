// ============================================================================
// src/utils/GasIaq.h
//
// The indoor-air-quality index the collector publishes as `iaq` (0..500,
// lower = cleaner, the BSEC convention) from a BME680/BME688's gas
// resistance and humidity, and the TVOC estimate it publishes as
// `tvoc_est` (ppb) from the same resistance. Shared by BME688Sensor (a
// wired sensor) and RemoteNodeSensor (a node's BME688, whose gas_resistance
// arrives over the network), so the two cannot drift apart.
//
// No Bosch BSEC: a self-calibrating clean-air baseline tracks the upper
// envelope of the MOX resistance, and the index combines a humidity score
// (optimal RH ≈ 40 %) with the current resistance ratio to that baseline.
//
// Header-only and free of Arduino, so a host test can include it.
// ============================================================================
#pragma once

#include <math.h>

struct GasIaq {
    /// Clean-air resistance ceiling (Ω); 0 = not seeded yet.
    float baseline = 0.0f;

    /// Fold one RAW (uncalibrated) gas reading into the baseline and return
    /// the index. Call once per new measurement: the baseline moves on every
    /// call, so feeding it the same reading twice counts it twice.
    /// `driftDown` false holds the slow downward drift: while a heater is
    /// still settling its low readings say nothing about the air, and a
    /// restored baseline would sink toward them.
    float update(float humidity, float rawGasOhm, bool driftDown = true) {
        // Baseline: rise quickly toward a higher (cleaner) resistance ceiling,
        // drift down very slowly to absorb sensor aging / ambient drift.
        if (baseline <= 0.0f)          baseline = rawGasOhm;                          // seed
        else if (rawGasOhm > baseline) baseline += (rawGasOhm - baseline) * 0.10f;
        else if (driftDown)            baseline += (rawGasOhm - baseline) * 0.0005f;
        if (baseline < 1.0f) baseline = 1.0f;

        // Humidity contribution (0..25): peaks in the 38–42 % comfort band.
        float humScore;
        if (humidity >= 38.0f && humidity <= 42.0f) humScore = 25.0f;
        else if (humidity < 38.0f)                  humScore = (humidity / 38.0f) * 25.0f;
        else                                        humScore = ((100.0f - humidity) / 58.0f) * 25.0f;
        if (humScore < 0.0f)  humScore = 0.0f;
        if (humScore > 25.0f) humScore = 25.0f;

        // Gas contribution (0..75): current resistance relative to clean baseline.
        float ratio = rawGasOhm / baseline;   // ≈1 when clean, <1 when polluted
        if (ratio > 1.0f) ratio = 1.0f;
        if (ratio < 0.0f) ratio = 0.0f;
        float gasScore = ratio * 75.0f;

        // Quality 0..100 (higher = cleaner) inverted to IAQ 0..500 (lower = cleaner).
        float iaq = (100.0f - (humScore + gasScore)) * 5.0f;
        if (iaq < 0.0f)   iaq = 0.0f;
        if (iaq > 500.0f) iaq = 500.0f;
        return iaq;
    }

    /// TVOC estimate (ppb) from a RAW gas reading against the current
    /// baseline; call after update() so the two agree. A MOX element follows
    /// Rs/R0 ≈ (C/C0)^-β, so C ≈ C0·(R0/Rs)^(1/β), with β ≈ 0.6 and clean air
    /// taken as C0 = 50 ppb. Rough: the element sums every reducing gas and
    /// has no absolute reference, so the trend is right and the level only
    /// approximate (a factor of two either way is unremarkable). Humidity is
    /// left out on purpose — it is half of what moves `iaq`, not a VOC.
    /// Examples: Rs = R0 → 50, R0/2 → ~160, R0/5 → ~730, R0/10 → ~2300.
    float tvocPpb(float rawGasOhm) const {
        if (!(baseline > 0.0f) || !(rawGasOhm > 0.0f)) return NAN;
        float ratio = baseline / rawGasOhm;     // ≥1 when polluted
        if (ratio < 1.0f) ratio = 1.0f;         // cleaner than the ceiling
        float ppb = TVOC_CLEAN_PPB * powf(ratio, 1.0f / TVOC_BETA);
        return ppb > TVOC_MAX_PPB ? TVOC_MAX_PPB : ppb;
    }

    static constexpr float TVOC_CLEAN_PPB = 50.0f;
    static constexpr float TVOC_BETA      = 0.6f;
    static constexpr float TVOC_MAX_PPB   = 5000.0f;
};
