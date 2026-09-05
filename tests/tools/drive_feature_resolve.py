#!/usr/bin/env python3
"""
drive_feature_resolve.py — what somebody types into the build form must mean
what they think it means.

WHY
---
The deploy GUI offers thirty checkboxes. A GitHub Actions form cannot: there is
a hard limit of ten inputs on a workflow_dispatch, and thirty booleans would be
most of a phone screen even if there were room. So the same choice arrives as a
line of text, and tools/features.py turns that line into the macro list the
compiler gets.

Every failure of that translation is silent and expensive. A name that quietly
matched nothing is a firmware missing exactly the feature it was built for,
with a green tick beside it and a two-minute build in between. A name that
matched the WRONG feature is worse: SENSOR_VEML6075_ENABLED (UV) and
SENSOR_VEML7700_ENABLED (lux) are one character apart, they are different
sensors on different addresses, and "veml" cannot be allowed to pick one.

And one distinction carries the whole thing: an empty result means "setup.h
decides", NOT "no features". There is no legitimate empty set — a build with
nothing to read does not compile — so the empty list can only mean the other
thing, and build_flags_for() returns no flags at all for it.

    python3 tests/tools/drive_feature_resolve.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from features import (  # noqa: E402
    build_flags_for,
    match_feature,
    optional_features,
    resolve_selection,
    sensor_features,
)

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def resolves_to(text: str, expected: list[str], what: str) -> None:
    macros, errors = resolve_selection(text)
    check(not errors and macros == expected,
          f"{what}: {text!r} -> {macros}{' errors=' + str(errors) if errors else ''}")


def refuses(text: str, needle: str, what: str) -> None:
    macros, errors = resolve_selection(text)
    check(bool(errors) and any(needle in e for e in errors),
          f"{what}: {text!r} -> {errors or 'accepted ' + str(macros)}")


print("What the build form accepts:")

# The names in setup.h always work, whatever the case.
resolves_to("SENSOR_BME280_ENABLED", ["SENSOR_BME280_ENABLED"], "the real macro")
resolves_to("sensor_bme280_enabled", ["SENSOR_BME280_ENABLED"], "  in any case")

# And so does what the thing is called. Nobody types _ENABLED on a phone.
resolves_to("bme280", ["SENSOR_BME280_ENABLED"], "the sensor's own name")
resolves_to("BME280", ["SENSOR_BME280_ENABLED"], "  in any case")
resolves_to("bme-280", ["SENSOR_BME280_ENABLED"], "  with punctuation of its own")
resolves_to("remote_nodes", ["FEATURE_REMOTE_NODES"], "an underscore name")
# A substring, asked of the matcher rather than through resolve_selection —
# "kindle" is a real and unique name, and a build that is ONLY a Kindle
# dashboard has nothing to put on it, which the reading-source guard below
# rightly refuses. Two different rules, tested separately.
check(match_feature("kindle")[0] == "FEATURE_KINDLE_DASHBOARD",
      "a unique substring: 'kindle' -> FEATURE_KINDLE_DASHBOARD")
check(match_feature("heater")[0] == "MODULE_HEATER_ENABLED",
      "  and 'heater' -> MODULE_HEATER_ENABLED")

# Several, in the order they were given, without duplicates.
resolves_to("bme280, kindle, bme280",
            ["SENSOR_BME280_ENABLED", "FEATURE_KINDLE_DASHBOARD"],
            "a list, deduplicated, in order")
resolves_to("bme280\nkindle",
            ["SENSOR_BME280_ENABLED", "FEATURE_KINDLE_DASHBOARD"],
            "  separated by whatever came out of the form")

print("\nThe two words that are not feature names:")

# THE DISTINCTION THE WHOLE FILE IS ABOUT.
macros, errors = resolve_selection("default")
check(macros == [] and not errors, "'default' resolves to nothing at all")
check(build_flags_for(macros) == "",
      "  and nothing at all means no flags, so setup.h keeps its own defaults")
check(resolve_selection("")[0] == [], "an empty box is the same as 'default'")
check(resolve_selection("   ")[0] == [], "  and so is a box with spaces in it")

everything = [f.macro for f in optional_features()]
resolves_to("all", everything, "'all' is every feature setup.h declares")
check(len(everything) > 20, f"  which is {len(everything)} of them, not a stale list")

print("\nTaking things back out:")
macros, errors = resolve_selection("all -sds011")
check(not errors and "SENSOR_SDS011_ENABLED" not in macros
      and len(macros) == len(everything) - 1,
      f"'all -sds011' is everything but that one ({len(macros)} of {len(everything)})")
macros, _ = resolve_selection("all -SENSOR_SDS011_ENABLED -pms5003")
check("SENSOR_SDS011_ENABLED" not in macros and "SENSOR_PMS5003_ENABLED" not in macros,
      "  and it takes both spellings, more than once")
macros, _ = resolve_selection("bme280 -bme280 kindle")
check(macros == ["FEATURE_KINDLE_DASHBOARD"],
      "  removing something that was added leaves the rest")

print("\nWhat it refuses, and why refusing beats guessing:")

# VEML6075 is UV and VEML7700 is lux: different sensors, different addresses,
# one character apart. Picking one would build the wrong firmware silently.
refuses("veml", "matches 2", "an ambiguous prefix names the candidates")
refuses("zm", "matches 2", "  and so does a two-letter one (ZMPT101B vs ZMCT103C)")
refuses("s", "matches", "  and one that matches most of the table")
refuses("bmp999", "not a feature", "a name that does not exist is not dropped quietly")
refuses("all -bmp999", "not a feature", "  including when it is being removed")

# setup.h has an #error for this. Saying it in a sentence, before a four-minute
# build, is the whole reason the check is here and not in the compiler.
refuses("mqtt", "nothing for the firmware to read",
        "a build with no reading source is refused in words")
refuses("kindle", "nothing for the firmware to read",
        "  even when what was asked for is perfectly real")

# ...and the ways out of it that setup.h itself allows.
for escape in ("bme280", "remote_nodes", "espnow"):
    macros, errors = resolve_selection(escape)
    check(not errors, f"  '{escape}' on its own is a build that can compile")

print("\nThe flags the compiler actually gets:")
flags = build_flags_for(resolve_selection("bme280 kindle")[0])
check(flags.startswith("-DFEATURE_SET_EXPLICIT "),
      "FEATURE_SET_EXPLICIT leads, or a cleared checkbox would mean nothing")
check("-DSENSOR_BME280_ENABLED" in flags and "-DFEATURE_KINDLE_DASHBOARD" in flags,
      "  followed by exactly what was chosen")
check("-DSENSOR_BME688_ENABLED" not in flags,
      "  and nothing that was not")

lmk_flags = build_flags_for(resolve_selection("espnow")[0], {"ESPNOW_LMK": "s3cr3t"})
check('-DESPNOW_LMK=\\"s3cr3t\\"' in lmk_flags,
      "a string-valued flag is quoted the way the compiler needs")

print("\nThe table it all reads from:")
check(len(sensor_features()) >= 15,
      f"setup.h still declares {len(sensor_features())} sensors")
for f in optional_features():
    macro, _ = match_feature(f.macro)
    if macro != f.macro:
        FAILURES.append(f"{f.macro} does not resolve to itself")
check(not [f for f in FAILURES if "does not resolve to itself" in f],
      "and every feature in it resolves to itself, whatever it is called")

print()
if FAILURES:
    print(f"FAIL: {len(FAILURES)} of {CHECKS} check(s) failed")
    for f in FAILURES:
        print("  - " + f)
    sys.exit(1)
print(f"OK: {CHECKS} checks — the build form says what it means")
