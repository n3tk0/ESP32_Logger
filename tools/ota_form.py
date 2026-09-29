#!/usr/bin/env python3
"""
ota_form.py — what the Build OTA Firmware form's dropdowns and checkboxes mean.

The form used to be one text box ("default", "all", "bme280 kindle -sds011")
because a workflow_dispatch took at most ten inputs. GitHub raised that to 25
in December 2025, which is room for a control per feature — almost. There are
thirty features and a board, so the sensors that are rarely fitted together
share a dropdown (none / one of them) and everything else is a checkbox:

    board             1 dropdown
    feature controls  23 (6 dropdowns, 17 checkboxes) covering all 30 features
    label             1 optional text box
                      --
                      25

FORM below is the one place that says which control turns on which macro. The
workflow YAML cannot import it — GitHub draws the form before any of our code
runs — so tests/tools/drive_ota_form.py holds the two against each other, and
against src/setup.h: every feature reachable by exactly one control, and the
form's defaults equal to what setup.h ships, so pressing Run without touching
anything builds the same firmware a plain `pio run` does.

    python3 tools/ota_form.py --resolve '<toJSON(inputs)>'
    python3 tools/ota_form.py --build-flags '<toJSON(inputs)>' [--espnow-lmk KEY]

An input missing from the JSON takes its form default. That is what a push or
pull_request run gets — `inputs` is empty there — so CI builds the defaults.
"""
from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from features import (  # noqa: E402
    build_flags_for,
    has_a_reading_source,
    optional_features,
)


@dataclass(frozen=True)
class Check:
    """A checkbox: ticked turns `macro` on."""
    name: str
    description: str
    macro: str
    default: bool


@dataclass(frozen=True)
class Pick:
    """A dropdown: each option turns on its macros; "none" turns on nothing."""
    name: str
    description: str
    options: dict[str, tuple[str, ...]] = field(default_factory=dict)
    default: str = "none"


#: In the order the form shows them. The board dropdown and the label box are
#: not features and live in the workflow alone.
FORM: tuple[Check | Pick, ...] = (
    Pick("temp_humidity", "Temperature / humidity / pressure sensor", {
        "none": (),
        "BME280": ("SENSOR_BME280_ENABLED",),
        "BME688": ("SENSOR_BME688_ENABLED",),
    }, default="BME280"),
    Check("ds18b20", "DS18B20 temperature probe (1-Wire)", "SENSOR_DS18B20_ENABLED", False),
    Pick("particulate", "Particulate matter (PM) sensor", {
        "none": (),
        "SDS011": ("SENSOR_SDS011_ENABLED",),
        "PMS5003": ("SENSOR_PMS5003_ENABLED",),
        "SPS30": ("SENSOR_SPS30_ENABLED",),
    }, default="SDS011"),
    Pick("air_quality", "Air quality (VOC) sensor", {
        "none": (),
        "ENS160": ("SENSOR_ENS160_ENABLED",),
        "SGP30": ("SENSOR_SGP30_ENABLED",),
    }),
    Check("co2", "CO2 sensor SCD40/SCD41", "SENSOR_SCD4X_ENABLED", False),
    Pick("light", "Light (lux) sensor", {
        "none": (),
        "BH1750": ("SENSOR_BH1750_ENABLED",),
        "VEML7700": ("SENSOR_VEML7700_ENABLED",),
    }),
    Check("uv", "UV sensor VEML6075", "SENSOR_VEML6075_ENABLED", False),
    Check("water_flow", "Water flow meter YF-S201 / YF-S403", "SENSOR_WATERFLOW_ENABLED", False),
    Check("rain", "Tipping-bucket rain gauge", "SENSOR_RAIN_ENABLED", False),
    Check("wind", "Anemometer + wind vane", "SENSOR_WIND_ENABLED", False),
    Check("soil", "Capacitive soil moisture", "SENSOR_SOIL_ENABLED", False),
    Check("distance", "HC-SR04 ultrasonic distance", "SENSOR_HCSR04_ENABLED", False),
    Pick("ac_mains", "AC mains metering", {
        "none": (),
        "voltage (ZMPT101B)": ("SENSOR_ZMPT101B_ENABLED",),
        "current (ZMCT103C)": ("SENSOR_ZMCT103C_ENABLED",),
        "voltage + current": ("SENSOR_ZMPT101B_ENABLED", "SENSOR_ZMCT103C_ENABLED"),
    }),
    Check("heater", "Enclosure heater", "MODULE_HEATER_ENABLED", False),
    Check("forecast", "Weather forecast (HTTPS)", "MODULE_FORECAST_ENABLED", False),
    Pick("nodes", "Receive readings from nodes", {
        "none": (),
        "HTTP": ("FEATURE_REMOTE_NODES",),
        # setup.h implies REMOTE_NODES from ESPNOW_INGEST; named here as well
        # so the run summary lists what the build really contains.
        "HTTP + ESP-NOW": ("FEATURE_REMOTE_NODES", "FEATURE_ESPNOW_INGEST"),
    }),
    Check("kindle", "Kindle e-ink dashboard", "FEATURE_KINDLE_DASHBOARD", False),
    Check("sd_card", "SD card storage", "FEATURE_SD_STORAGE", True),
    Check("mqtt", "Export: MQTT", "EXPORT_MQTT_ENABLED", True),
    Check("http_post", "Export: generic HTTP POST", "EXPORT_HTTP_ENABLED", True),
    Check("sensor_community", "Export: sensor.community", "EXPORT_SENSORCOMMUNITY_ENABLED", True),
    Check("opensensemap", "Export: openSenseMap", "EXPORT_OPENSENSEMAP_ENABLED", True),
    Check("webhook", "Export: webhook (Discord/Slack/IFTTT)", "EXPORT_WEBHOOK_ENABLED", True),
)


def _truthy(value) -> bool:
    # toJSON(inputs) gives a real boolean for a checkbox; the REST API and
    # `gh workflow run -f` send the string "true"/"false".
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() == "true"


def macros_from_inputs(inputs: dict) -> tuple[list[str], list[str]]:
    """The form's values -> (macros in setup.h order, errors)."""
    chosen: set[str] = set()
    errors: list[str] = []
    for control in FORM:
        value = inputs.get(control.name)
        if isinstance(control, Check):
            on = control.default if value is None or value == "" else _truthy(value)
            if on:
                chosen.add(control.macro)
        else:
            pick = control.default if value is None or value == "" else str(value)
            if pick not in control.options:
                errors.append(f"{control.name}: {pick!r} is not one of "
                              f"{', '.join(control.options)}")
                continue
            chosen.update(control.options[pick])

    if not errors and not has_a_reading_source(chosen):
        errors.append("this build has no sensor and does not receive from nodes, "
                      "so it would have nothing to read — pick at least one "
                      "sensor, or HTTP under \"Receive readings from nodes\"")

    ordered = [f.macro for f in optional_features() if f.macro in chosen]
    return ordered, errors


def cli(argv: list[str]) -> int:
    if not argv or argv[0] not in ("--resolve", "--build-flags"):
        print("usage: ota_form.py --resolve JSON | --build-flags JSON "
              "[--espnow-lmk KEY]", file=sys.stderr)
        return 2
    verb = argv[0]
    try:
        inputs = json.loads(argv[1]) if len(argv) > 1 and argv[1].strip() else {}
    except json.JSONDecodeError as e:
        print(f"ota_form: the inputs are not JSON: {e}", file=sys.stderr)
        return 2
    if not isinstance(inputs, dict):
        inputs = {}

    lmk = ""
    if "--espnow-lmk" in argv:
        i = argv.index("--espnow-lmk")
        lmk = argv[i + 1] if i + 1 < len(argv) else ""

    macros, errors = macros_from_inputs(inputs)
    if errors:
        for e in errors:
            print(f"ota_form: {e}", file=sys.stderr)
        return 2

    if verb == "--resolve":
        for m in macros:
            print(m)
        return 0

    extra = {}
    if lmk and "FEATURE_ESPNOW_INGEST" in macros:
        extra["ESPNOW_LMK"] = lmk
    print(build_flags_for(macros, extra))
    return 0


if __name__ == "__main__":
    sys.exit(cli(sys.argv[1:]))
